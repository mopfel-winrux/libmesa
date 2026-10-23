/// @file
///
/// mesa-guestbook: a guestbook any ship can sign, as a libmesa app.
///
///   mesa-guestbook [identity] -s state.db [-H webhook-url] [-W [addr:]port]
///
/// identity and state: see tools/cli.h (-F ~ship, -k keyfile, -c, -b, -p)
///
/// From any ship's dojo:
///
///   :~our/guestbook 'hello!'        ::  sign it (a %noun poke of a cord)
///
/// and from libmesa:
///
///   mesa-ping ... ~our guestbook 'hello!'
///   mesa-ping ... -w /entries ~our guestbook    ::  [id=@ud who=@p text=@t]
///   mesa-ping ... -x /g/x/3/guestbook//1/entry ~our   ::  entry #3
///
/// Everything lives in the engine's own SQLite database, so each signature
/// is recorded exactly once: the insert, the facts to subscribers and the
/// ack commit together, or not at all. With -H, each new entry is also
/// POSTed as JSON to a webhook, through an outbox table drained after the
/// commit: at least once, with an id the receiver can deduplicate on.
/// With -W, it serves a page showing the guestbook at http://addr:port/
/// (and the entries as JSON at /entries.json); addr defaults to 127.0.0.1.

#include "mesa/engine.h"
#include "mesa/gall.h"
#include "mesa/host.h"

#include "../tools/cli.h"

#include <curl/curl.h>
#include <sqlite3.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <uv.h>

#define GB_AGENT    "guestbook"
#define GB_MAX      1024          //  longest entry, bytes
#define GB_BACKLOG  10            //  entries a new subscriber gets

typedef struct gb {
  mesa_host*   hos;
  mesa_engine* eng;
  sqlite3*     db;
  const char*  hok;               //  webhook url
  uv_check_t   chk;               //  drain the outbox after each commit
  uv_timer_t   tim;               //  outbox retry backoff
  int          busy;              //  a POST in flight
  uint64_t     wait_ms;
  char*        our;               //  our @p, for the page
  uv_tcp_t     web;               //  -W: the local web page
} gb;

static gb gb_u;

/* sql helpers
*/
static void
_sql_die(const char* what)
{
  fprintf(stderr, "guestbook: %s: %s\n", what, sqlite3_errmsg(gb_u.db));
  exit(1);
}

static sqlite3_stmt*
_sql(const char* sql)
{
  sqlite3_stmt* stm;
  if ( SQLITE_OK != sqlite3_prepare_v2(gb_u.db, sql, -1, &stm, 0) ) {
    _sql_die(sql);
  }
  return stm;
}

static mesa_noun
_ship_noun(mesa_ship who)
{
  uint8_t buf[16];
  for ( int i = 0; i < 8; i++ ) {
    buf[i]     = (uint8_t)(who.lo >> (8 * i));
    buf[i + 8] = (uint8_t)(who.hi >> (8 * i));
  }
  return mesa_atom_bytes(16, buf);
}

static mesa_noun
_error(const char* tag, const char* msg)
{
  //  [tag ~[leaf+msg]]
  mesa_noun tap = mesa_atom_u64(0);
  for ( size_t i = strlen(msg); i--; ) {
    tap = mesa_cell(mesa_atom_u64((uint8_t)msg[i]), tap);
  }
  return mesa_cell(mesa_atom_cord(tag),
                   mesa_cell(mesa_cell(mesa_atom_cord("leaf"), tap),
                             mesa_atom_u64(0)));
}

/* _entry(): entry #id as [id who text], or MESA_NONE
*/
static mesa_noun
_entry(int64_t id)
{
  sqlite3_stmt* stm = _sql("SELECT ship, text FROM gb_entry WHERE id = ?");
  mesa_noun     ent = MESA_NONE;
  mesa_ship     who;

  sqlite3_bind_int64(stm, 1, id);
  if ( (SQLITE_ROW == sqlite3_step(stm))
     && mesa_ship_parse((const char*)sqlite3_column_text(stm, 0), &who) )
  {
    ent = mesa_trel(mesa_atom_u64((uint64_t)id), _ship_noun(who),
                    mesa_atom_cord((const char*)sqlite3_column_text(stm, 1)));
  }
  sqlite3_finalize(stm);
  return ent;
}

static void
_json_str(char* out, size_t cap, const char* str)
{
  size_t n = 0;
  for ( ; *str && n + 7 < cap; str++ ) {
    unsigned char c = (unsigned char)*str;
    if ( c == '"' || c == '\\' ) { out[n++] = '\\'; out[n++] = (char)c; }
    else if ( c < 0x20 ) n += (size_t)snprintf(out + n, cap - n, "\\u%04x", c);
    else out[n++] = (char)c;
  }
  out[n] = 0;
}

/* _sign(): record an entry and tell everyone (inside the event's txn)
*/
static void
_sign(mesa_ship who, const char* text)
{
  char*         src = mesa_ship_render(who);
  sqlite3_stmt* stm = _sql("INSERT INTO gb_entry (ship, text, at) VALUES (?, ?, ?)");
  int64_t       id;

  sqlite3_bind_text(stm, 1, src, -1, SQLITE_TRANSIENT);
  sqlite3_bind_text(stm, 2, text, -1, SQLITE_TRANSIENT);
  sqlite3_bind_int64(stm, 3, (int64_t)time(0));
  if ( SQLITE_DONE != sqlite3_step(stm) ) _sql_die("insert");
  sqlite3_finalize(stm);
  id = sqlite3_last_insert_rowid(gb_u.db);

  printf("#%lld %s: %s\n", (long long)id, src, text);
  fflush(stdout);

  //  a fact to each subscriber
  //
  stm = _sql("SELECT ship, bone FROM gb_sub");
  while ( SQLITE_ROW == sqlite3_step(stm) ) {
    mesa_ship sub;
    if ( mesa_ship_parse((const char*)sqlite3_column_text(stm, 0), &sub) ) {
      mesa_gall_fact(gb_u.eng, sub, (uint64_t)sqlite3_column_int64(stm, 1),
                     "noun", _entry(id));
    }
  }
  sqlite3_finalize(stm);

  //  and to the webhook, after we commit
  //
  if ( gb_u.hok ) {
    char txt[GB_MAX * 6 + 1], bod[GB_MAX * 6 + 256];
    _json_str(txt, sizeof(txt), text);
    snprintf(bod, sizeof(bod), "{\"id\":%lld,\"ship\":\"%s\",\"text\":\"%s\"}",
             (long long)id, src, txt);
    stm = _sql("INSERT INTO gb_outbox (body) VALUES (?)");
    sqlite3_bind_text(stm, 1, bod, -1, SQLITE_TRANSIENT);
    if ( SQLITE_DONE != sqlite3_step(stm) ) _sql_die("outbox");
    sqlite3_finalize(stm);
  }
  free(src);
}

/* engine hooks
*/
static mesa_plea_result
_plea(void* ctx, mesa_ship her, uint64_t bone, mesa_noun vane,
      mesa_noun path, mesa_noun payload, mesa_noun* error)
{
  mesa_gall_request req;

  switch ( mesa_gall_request_parse(vane, path, payload, &req) ) {
    case MESA_GALL_SYSTEM: return MESA_ACK;

    case MESA_GALL_POKE: {
      char* txt;
      if ( strcmp(req.agent, GB_AGENT) ) break;
      if ( !mesa_is_atom(req.noun) || mesa_met3(req.noun) > GB_MAX ) {
        *error = _error("bad-entry", "sign with a cord of at most 1024 bytes");
        return MESA_NACK;
      }
      txt = mesa_atom_get_cord(req.noun);
      if ( strlen(txt) != mesa_met3(req.noun) || !txt[0] ) {
        free(txt);
        *error = _error("bad-entry", "sign with a non-empty cord");
        return MESA_NACK;
      }
      _sign(her, txt);
      free(txt);
      return MESA_ACK;
    }

    case MESA_GALL_WATCH: {
      char*         src;
      sqlite3_stmt* stm;
      if ( strcmp(req.agent, GB_AGENT) ) break;
      if ( !mesa_is_cell(req.path) || !mesa_is_atom(mesa_head(req.path))
         || (MESA_NONE == mesa_head(req.path)) )
      {
        *error = _error("bad-path", "watch /entries");
        return MESA_NACK;
      }
      {
        char* seg = mesa_atom_get_cord(mesa_head(req.path));
        int   ok  = !strcmp(seg, "entries") && mesa_is_atom(mesa_tail(req.path));
        free(seg);
        if ( !ok ) {
          *error = _error("bad-path", "watch /entries");
          return MESA_NACK;
        }
      }

      src = mesa_ship_render(her);
      stm = _sql("INSERT OR REPLACE INTO gb_sub (ship, bone) VALUES (?, ?)");
      sqlite3_bind_text(stm, 1, src, -1, SQLITE_TRANSIENT);
      sqlite3_bind_int64(stm, 2, (int64_t)bone);
      if ( SQLITE_DONE != sqlite3_step(stm) ) _sql_die("subscribe");
      sqlite3_finalize(stm);
      printf("%s subscribed\n", src);
      fflush(stdout);
      free(src);

      //  the latest entries, oldest first
      //
      stm = _sql("SELECT id FROM (SELECT id FROM gb_entry ORDER BY id DESC LIMIT ?) "
                 "ORDER BY id");
      sqlite3_bind_int(stm, 1, GB_BACKLOG);
      while ( SQLITE_ROW == sqlite3_step(stm) ) {
        mesa_gall_fact(gb_u.eng, her, bone, "noun",
                       _entry(sqlite3_column_int64(stm, 0)));
      }
      sqlite3_finalize(stm);
      return MESA_ACK;
    }

    case MESA_GALL_LEAVE: {
      char*         src = mesa_ship_render(her);
      sqlite3_stmt* stm = _sql("DELETE FROM gb_sub WHERE ship = ? AND bone = ?");
      sqlite3_bind_text(stm, 1, src, -1, SQLITE_TRANSIENT);
      sqlite3_bind_int64(stm, 2, (int64_t)bone);
      if ( SQLITE_DONE != sqlite3_step(stm) ) _sql_die("leave");
      sqlite3_finalize(stm);
      printf("%s left\n", src);
      fflush(stdout);
      free(src);
      return MESA_ACK;
    }

    default: break;
  }
  //  an agent we don't run: tell them to stop asking (a request for
  //  another vane is dropped: no answer)
  //
  return MESA_FLUB;
}

static void
_breach(void* ctx, mesa_ship her)
{
  //  their flows are gone, and so are their subscriptions
  //
  char*         src = mesa_ship_render(her);
  sqlite3_stmt* stm = _sql("DELETE FROM gb_sub WHERE ship = ?");
  sqlite3_bind_text(stm, 1, src, -1, SQLITE_TRANSIENT);
  if ( SQLITE_DONE != sqlite3_step(stm) ) _sql_die("breach");
  sqlite3_finalize(stm);
  free(src);
}

/* _peek(): entry #n is /g/x/<n>/guestbook//1/entry, forever
*/
static mesa_noun
_peek(void* ctx, const mesa_ship* her, const char* path)
{
  long long id;
  int       end = 0;

  if ( (1 == sscanf(path, "/g/x/%lld/" GB_AGENT "//1/entry%n", &id, &end))
     && end && !path[end] && (id > 0) )
  {
    mesa_noun ent = _entry(id);
    if ( MESA_NONE != ent ) {
      return mesa_cell(mesa_atom_cord("noun"), ent);
    }
  }
  return MESA_NONE;
}

/* the webhook outbox: POST the oldest entry off the loop thread; delete it
** once delivered, back off if not
*/
typedef struct gb_post {
  uv_work_t wok;
  int64_t   id;
  char*     body;
  bool      ok;
} gb_post;

static void _drain(void);

static void
_post_work(uv_work_t* wok)
{
  gb_post* pos = wok->data;
  pos->ok = mesa_host_post(gb_u.hok, "application/json", pos->body, strlen(pos->body));
}

static void
_retry_cb(uv_timer_t* tim)
{
  _drain();
}

static void
_post_done(uv_work_t* wok, int sas)
{
  gb_post* pos = wok->data;

  gb_u.busy = 0;
  if ( pos->ok ) {
    sqlite3_stmt* stm;
    //  our own transaction: the engine's store isn't in one between events
    //
    if ( MESA_OK != mesa_txn_begin(gb_u.eng) ) _sql_die("begin");
    stm = _sql("DELETE FROM gb_outbox WHERE id = ?");
    sqlite3_bind_int64(stm, 1, pos->id);
    if ( SQLITE_DONE != sqlite3_step(stm) ) _sql_die("outbox delete");
    sqlite3_finalize(stm);
    if ( MESA_OK != mesa_txn_commit(gb_u.eng) ) _sql_die("commit");
    gb_u.wait_ms = 0;
    _drain();
  }
  else {
    gb_u.wait_ms = gb_u.wait_ms ? (gb_u.wait_ms * 2) : 1000;
    if ( gb_u.wait_ms > 300000 ) gb_u.wait_ms = 300000;
    uv_timer_start(&gb_u.tim, _retry_cb, gb_u.wait_ms, 0);
  }
  free(pos->body);
  free(pos);
}

static void
_drain(void)
{
  sqlite3_stmt* stm;
  gb_post*      pos;

  if ( !gb_u.hok || gb_u.busy || uv_is_active((uv_handle_t*)&gb_u.tim) ) return;

  stm = _sql("SELECT id, body FROM gb_outbox ORDER BY id LIMIT 1");
  if ( SQLITE_ROW != sqlite3_step(stm) ) {
    sqlite3_finalize(stm);
    return;
  }
  pos = calloc(1, sizeof(*pos));
  pos->id   = sqlite3_column_int64(stm, 0);
  pos->body = strdup((const char*)sqlite3_column_text(stm, 1));
  pos->wok.data = pos;
  sqlite3_finalize(stm);

  gb_u.busy = 1;
  uv_queue_work(mesa_host_loop(gb_u.hos), &pos->wok, _post_work, _post_done);
}

static void
_check_cb(uv_check_t* chk)
{
  //  runs after each loop iteration's I/O: every event has committed
  //
  _drain();
}

/* the local web page (-W): a page that polls /entries.json
*/
static const char gb_page[] =
"<!doctype html>\n"
"<html lang=\"en\"><head><meta charset=\"utf-8\">\n"
"<meta name=\"viewport\" content=\"width=device-width,initial-scale=1\">\n"
"<title>Guestbook</title>\n"
"<style>\n"
":root{--bg:#f6f5f1;--card:#fff;--ink:#1d1c1a;--mute:#76736b;--line:#e4e1d8;--acc:#3b5bdb}\n"
"@media (prefers-color-scheme:dark){:root{--bg:#141413;--card:#1e1e1c;--ink:#ecebe6;"
"--mute:#9a978f;--line:#2e2d2a;--acc:#8ea2ff}}\n"
"*{box-sizing:border-box}\n"
"body{margin:0;background:var(--bg);color:var(--ink);"
"font:16px/1.5 ui-sans-serif,system-ui,-apple-system,sans-serif}\n"
"main{max-width:680px;margin:0 auto;padding:40px 16px 64px}\n"
"h1{font-size:28px;margin:0 0 4px;letter-spacing:-.01em}\n"
".who{font:13px/1.4 ui-monospace,SFMono-Regular,Menlo,monospace;color:var(--mute);"
"word-break:break-all}\n"
".meta{display:flex;gap:12px;align-items:center;margin:18px 0 22px;color:var(--mute);font-size:14px}\n"
".dot{width:8px;height:8px;border-radius:50%;background:#2f9e44}\n"
".dot.off{background:#c92a2a}\n"
".hint{background:var(--card);border:1px solid var(--line);border-radius:10px;"
"padding:12px 14px;font-size:14px;color:var(--mute);margin-bottom:22px}\n"
".hint code{font:12.5px ui-monospace,Menlo,monospace;color:var(--ink);word-break:break-all}\n"
"ol{list-style:none;margin:0;padding:0;display:flex;flex-direction:column;gap:10px}\n"
"li{background:var(--card);border:1px solid var(--line);border-radius:10px;padding:14px 16px}\n"
"li.new{animation:hi 1.6s ease-out}\n"
"@keyframes hi{from{border-color:var(--acc)}}\n"
".row{display:flex;justify-content:space-between;gap:12px;font-size:13px;color:var(--mute)}\n"
".ship{font-family:ui-monospace,Menlo,monospace;color:var(--acc);word-break:break-all}\n"
".text{margin-top:6px;white-space:pre-wrap;word-break:break-word;font-size:17px}\n"
".empty{text-align:center;color:var(--mute);padding:40px 0}\n"
"</style></head><body><main>\n"
"<h1>Guestbook</h1>\n"
"<div class=\"who\" id=\"our\"></div>\n"
"<div class=\"meta\"><span class=\"dot\" id=\"dot\"></span><span id=\"count\">loading</span></div>\n"
"<div class=\"hint\">Sign it from any ship's dojo: <code id=\"cmd\"></code></div>\n"
"<ol id=\"list\"></ol>\n"
"</main><script>\n"
"const seen=new Set();let first=true;\n"
"function ago(t){if(!t)return'';const s=Math.max(0,Date.now()/1000-t);"
"if(s<60)return'just now';if(s<3600)return Math.floor(s/60)+'m ago';"
"if(s<86400)return Math.floor(s/3600)+'h ago';return new Date(t*1000).toLocaleDateString()}\n"
"function el(tag,cls,txt){const e=document.createElement(tag);if(cls)e.className=cls;"
"if(txt!=null)e.textContent=txt;return e}\n"
"async function load(){try{\n"
" const r=await fetch('entries.json',{cache:'no-store'});const d=await r.json();\n"
" document.getElementById('our').textContent=d.ship;\n"
" document.getElementById('cmd').textContent=':'+d.ship+\"/guestbook 'hello!'\";\n"
" const n=d.entries.length;\n"
" document.getElementById('count').textContent=n+(n==1?' signature':' signatures');\n"
" document.getElementById('dot').className='dot';\n"
" const list=document.getElementById('list');list.replaceChildren();\n"
" if(!n)list.append(el('div','empty','No signatures yet.'));\n"
" for(const e of d.entries){const li=el('li',(!first&&!seen.has(e.id))?'new':'');\n"
"  const row=el('div','row');row.append(el('span','ship',e.ship),"
"el('span','',ago(e.at)+'  #'+e.id));\n"
"  li.append(row,el('div','text',e.text));list.append(li);seen.add(e.id)}\n"
" first=false;\n"
"}catch(x){document.getElementById('dot').className='dot off';"
"document.getElementById('count').textContent='offline'}}\n"
"load();setInterval(load,2000);\n"
"</script></body></html>\n";

typedef struct gb_conn {
  uv_tcp_t tcp;
  char     buf[4096];
  size_t   len;
  int      done;
} gb_conn;

typedef struct gb_out {
  uv_write_t wri;
  char*      buf;
} gb_out;

static void
_web_closed(uv_handle_t* han)
{
  free(han->data);
}

static void
_web_wrote(uv_write_t* wri, int sas)
{
  gb_out* out = wri->data;
  uv_close((uv_handle_t*)wri->handle, _web_closed);
  free(out->buf);
  free(out);
}

static void
_web_send(gb_conn* con, const char* sta, const char* typ, const char* bod, size_t len)
{
  gb_out*  out = calloc(1, sizeof(*out));
  size_t   cap = len + 256;
  int      hed;
  uv_buf_t buf;

  out->buf = malloc(cap);
  hed = snprintf(out->buf, cap,
                 "HTTP/1.1 %s\r\nContent-Type: %s\r\nContent-Length: %zu\r\n"
                 "Cache-Control: no-store\r\nConnection: close\r\n\r\n",
                 sta, typ, len);
  memcpy(out->buf + hed, bod, len);
  buf = uv_buf_init(out->buf, (unsigned)(hed + len));
  out->wri.data = out;
  uv_write(&out->wri, (uv_stream_t*)&con->tcp, &buf, 1, _web_wrote);
}

/* _web_entries(): {"ship":..., "entries":[{id, ship, text, at}...]}, newest first
*/
static char*
_web_entries(size_t* len)
{
  sqlite3_stmt* stm = _sql("SELECT id, ship, text, at FROM gb_entry ORDER BY id DESC LIMIT 500");
  size_t        cap = 4096, n = 0;
  char*         out = malloc(cap);

  n += (size_t)snprintf(out, cap, "{\"ship\":\"%s\",\"entries\":[", gb_u.our);
  while ( SQLITE_ROW == sqlite3_step(stm) ) {
    const char* txt = (const char*)sqlite3_column_text(stm, 2);
    size_t      max = strlen(txt) * 6 + 256;
    char*       esc = malloc(strlen(txt) * 6 + 1);

    _json_str(esc, strlen(txt) * 6 + 1, txt);
    if ( n + max + 8 > cap ) {
      cap = (n + max) * 2;
      out = realloc(out, cap);
    }
    n += (size_t)snprintf(out + n, cap - n, "%s{\"id\":%lld,\"ship\":\"%s\",\"text\":\"%s\",\"at\":%lld}",
                          (n && out[n - 1] != '[') ? "," : "",
                          (long long)sqlite3_column_int64(stm, 0),
                          (const char*)sqlite3_column_text(stm, 1), esc,
                          (long long)sqlite3_column_int64(stm, 3));
    free(esc);
  }
  sqlite3_finalize(stm);
  if ( n + 4 > cap ) out = realloc(out, n + 4);
  n += (size_t)snprintf(out + n, 4, "]}");
  *len = n;
  return out;
}

static void
_web_alloc(uv_handle_t* han, size_t sug, uv_buf_t* buf)
{
  gb_conn* con = han->data;
  *buf = uv_buf_init(con->buf + con->len, (unsigned)(sizeof(con->buf) - con->len - 1));
}

static void
_web_read(uv_stream_t* str, ssize_t nrd, const uv_buf_t* buf)
{
  gb_conn* con = str->data;

  if ( nrd < 0 || con->done ) {
    if ( !con->done ) uv_close((uv_handle_t*)str, _web_closed);
    return;
  }
  con->len += (size_t)nrd;
  con->buf[con->len] = 0;
  if ( !strstr(con->buf, "\r\n\r\n") ) {
    if ( con->len >= sizeof(con->buf) - 1 ) {
      con->done = 1;
      uv_read_stop(str);
      _web_send(con, "431 Too Large", "text/plain", "", 0);
    }
    return;
  }
  con->done = 1;
  uv_read_stop(str);

  if ( !strncmp(con->buf, "GET / ", 6) || !strncmp(con->buf, "GET /index.html ", 16) ) {
    _web_send(con, "200 OK", "text/html; charset=utf-8", gb_page, sizeof(gb_page) - 1);
  }
  else if ( !strncmp(con->buf, "GET /entries.json ", 18) ) {
    size_t len;
    char*  bod = _web_entries(&len);
    _web_send(con, "200 OK", "application/json", bod, len);
    free(bod);
  }
  else {
    _web_send(con, "404 Not Found", "text/plain", "not found\n", 10);
  }
}

static void
_web_accept(uv_stream_t* srv, int sas)
{
  gb_conn* con;

  if ( sas < 0 ) return;
  con = calloc(1, sizeof(*con));
  uv_tcp_init(srv->loop, &con->tcp);
  con->tcp.data = con;
  if ( uv_accept(srv, (uv_stream_t*)&con->tcp) ) {
    uv_close((uv_handle_t*)&con->tcp, _web_closed);
    return;
  }
  uv_read_start((uv_stream_t*)&con->tcp, _web_alloc, _web_read);
}

static int
_web_start(const char* arg)
{
  struct sockaddr_in add;
  char               adr[64] = "127.0.0.1";
  const char*        col = strrchr(arg, ':');
  int                port;

  if ( col ) {
    snprintf(adr, sizeof(adr), "%.*s", (int)(col - arg), arg);
    arg = col + 1;
  }
  port = atoi(arg);
  uv_tcp_init(mesa_host_loop(gb_u.hos), &gb_u.web);
  if (  (port <= 0) || (port > 65535)
     || uv_ip4_addr(adr, port, &add)
     || uv_tcp_bind(&gb_u.web, (const struct sockaddr*)&add, 0)
     || uv_listen((uv_stream_t*)&gb_u.web, 64, _web_accept) )
  {
    fprintf(stderr, "guestbook: can't serve on %s:%d\n", adr, port);
    return 0;
  }
  printf("guestbook page: http://%s:%d/\n", adr, port);
  fflush(stdout);
  return 1;
}

static void
_usage(void)
{
  fprintf(stderr, "usage: mesa-guestbook " CLI_USAGE " [-H webhook-url] [-W [addr:]port]\n");
  exit(2);
}

int
main(int argc, char* argv[])
{
  cli cli_u = {0};
  int err;
  const char* web = 0;

  curl_global_init(CURL_GLOBAL_DEFAULT);

  for ( int i = 1; i < argc; i++ ) {
    if      ( cli_option(&cli_u, argc, argv, &i) ) {}
    else if ( !strcmp(argv[i], "-H") && i + 1 < argc ) { gb_u.hok = argv[++i]; }
    else if ( !strcmp(argv[i], "-W") && i + 1 < argc ) { web = argv[++i]; }
    else _usage();
  }
  if ( !cli_u.db ) {
    fprintf(stderr, "guestbook: -s state.db is required\n");
    _usage();
  }
  if ( !cli_open(&cli_u) ) return 2;

  //  our tables live in the engine's database, so they commit with it
  //
  gb_u.db = mesa_store_sqlite_db(cli_u.cfg.engine.store);
  if ( SQLITE_OK != sqlite3_exec(gb_u.db,
         "CREATE TABLE IF NOT EXISTS gb_entry ("
         "  id INTEGER PRIMARY KEY, ship TEXT NOT NULL, text TEXT NOT NULL);"
         "CREATE TABLE IF NOT EXISTS gb_sub ("
         "  ship TEXT NOT NULL, bone INTEGER NOT NULL, PRIMARY KEY (ship, bone));"
         "CREATE TABLE IF NOT EXISTS gb_outbox ("
         "  id INTEGER PRIMARY KEY, body TEXT NOT NULL);", 0, 0, 0) )
  {
    _sql_die("schema");
  }
  //  when each entry was signed (added after the first version)
  sqlite3_exec(gb_u.db, "ALTER TABLE gb_entry ADD COLUMN at INTEGER", 0, 0, 0);

  cli_u.cfg.engine.hooks = (mesa_hooks){ .plea = _plea, .peek = _peek,
                                         .breach = _breach };

  if ( MESA_OK != (err = mesa_host_open(&cli_u.cfg, &gb_u.hos)) ) {
    fprintf(stderr, "can't start: %d\n", err);
    return 2;
  }
  gb_u.eng = mesa_host_engine(gb_u.hos);

  uv_check_init(mesa_host_loop(gb_u.hos), &gb_u.chk);
  uv_check_start(&gb_u.chk, _check_cb);
  uv_timer_init(mesa_host_loop(gb_u.hos), &gb_u.tim);
  _drain();

  gb_u.our = mesa_ship_render(cli_u.ide.who);
  if ( web && !_web_start(web) ) return 2;

  {
    char*         who = mesa_ship_render(cli_u.ide.who);
    sqlite3_stmt* stm = _sql("SELECT count(*) FROM gb_entry");
    sqlite3_step(stm);
    printf("guestbook of %s: %lld entries\n", who,
           (long long)sqlite3_column_int64(stm, 0));
    fflush(stdout);
    sqlite3_finalize(stm);
    free(who);
  }

  mesa_host_run(gb_u.hos);
  mesa_host_close(gb_u.hos);
  mesa_store_free(cli_u.cfg.engine.store);
  return 0;
}

/// @file
///
/// mesa-guestbook: a guestbook any ship can sign, as a libmesa app.
///
///   mesa-guestbook [identity] -s state.db [-H webhook-url]
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

#include "mesa/engine.h"
#include "mesa/gall.h"
#include "mesa/host.h"

#include "../tools/cli.h"

#include <curl/curl.h>
#include <sqlite3.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
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
  sqlite3_stmt* stm = _sql("INSERT INTO gb_entry (ship, text) VALUES (?, ?)");
  int64_t       id;

  sqlite3_bind_text(stm, 1, src, -1, SQLITE_TRANSIENT);
  sqlite3_bind_text(stm, 2, text, -1, SQLITE_TRANSIENT);
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
  *error = _error("no-agent", "only %guestbook lives here");
  return MESA_NACK;
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

static void
_usage(void)
{
  fprintf(stderr, "usage: mesa-guestbook " CLI_USAGE " [-H webhook-url]\n");
  exit(2);
}

int
main(int argc, char* argv[])
{
  cli cli_u = {0};
  int err;

  curl_global_init(CURL_GLOBAL_DEFAULT);

  for ( int i = 1; i < argc; i++ ) {
    if      ( cli_option(&cli_u, argc, argv, &i) ) {}
    else if ( !strcmp(argv[i], "-H") && i + 1 < argc ) { gb_u.hok = argv[++i]; }
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

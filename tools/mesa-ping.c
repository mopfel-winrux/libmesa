/// @file
///
/// mesa-ping: talk to a ship's agents from the command line.
///
///   mesa-ping [identity] [options] ~ship agent [text]
///
/// identity (one of):
///   -F ~ship        fake ship (to talk to `urbit -F` ships on localhost)
///   -k keyfile      a planet, star, galaxy or moon keyfile (@uw)
///   -c              mine a fresh comet (keyfile saved to <state>.key)
///
/// options:
///   -s state.db     persistent state (SQLite); default: in memory
///   -b              this identity breached (allow an empty state)
///   -w /path        watch /path on the agent and print facts
///   -x /path        remote scry /path (e.g. /g/x/1/agent//1/foo)
///   -p port         UDP port to bind
///   -P ~ship        print ~ship's azimuth point and exit
///
/// Without -w or -x, pokes [agent] with [%noun text] and waits for the ack.

#include "mesa/engine.h"
#include "mesa/gall.h"
#include "mesa/host.h"
#include "mesa/ident.h"

#include <curl/curl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/random.h>
#include <uv.h>

static mesa_host* hos;
static mesa_ship  her;
static uint64_t   bon_d;
static int        ret_i = 1;
static const char* wat_c;

static void
_quit(int code)
{
  ret_i = code;
  mesa_host_stop(hos);
}

static void
_print(const char* pre, mesa_noun som)
{
  //  cords print as text; anything else as its jam size
  //
  if ( mesa_is_atom(som) && mesa_met3(som) < 4096 ) {
    char* str = mesa_atom_get_cord(som);
    int   txt = 1;
    for ( char* c = str; *c; c++ ) if ( (*c < 32) && (*c != '\n') ) txt = 0;
    if ( txt ) { printf("%s'%s'\n", pre, str); free(str); return; }
    free(str);
  }
  {
    size_t   len;
    uint8_t* jam = mesa_jam_bytes(som, &len);
    printf("%s<noun, %zu bytes jammed>\n", pre, len);
    free(jam);
  }
}

/* _tank(): print a tank on one line (+ram)
*/
static void
_tape(mesa_noun tap)
{
  for ( ; mesa_is_cell(tap); tap = mesa_tail(tap) ) {
    uint64_t c;
    if ( mesa_atom_get_u64(mesa_head(tap), &c) && c < 256 ) putchar((int)c);
  }
}

static void
_tank(mesa_noun tan, int dep)
{
  uint64_t tag;

  if ( dep > 32 || !mesa_is_cell(tan) || !mesa_atom_get_u64(mesa_head(tan), &tag) ) return;
  if ( tag == 0x6661656c ) {  //  %leaf
    _tape(mesa_tail(tan));
  }
  else if ( tag == 0x65736f72 || tag == 0x6d6c6170 ) {  //  %rose [p q r], %palm [p q r s]
    mesa_noun pre = mesa_head(mesa_tail(tan));
    mesa_noun sep = mesa_head(pre);
    mesa_noun ope = mesa_head(mesa_tail(pre));
    mesa_noun clo = mesa_tail(mesa_tail(pre));
    if ( tag == 0x6d6c6170 ) {
      clo = mesa_tail(mesa_tail(clo));
      sep = mesa_head(mesa_tail(mesa_tail(pre)));
    }
    _tape(ope);
    for ( mesa_noun t = mesa_tail(mesa_tail(tan)); mesa_is_cell(t); t = mesa_tail(t) ) {
      _tank(mesa_head(t), dep + 1);
      if ( mesa_is_cell(mesa_tail(t)) ) _tape(sep);
    }
    _tape(clo);
  }
}

static void
_done(void* ctx, mesa_ship who, uint64_t bone, mesa_noun error)
{
  if ( MESA_NONE == error ) {
    printf(wat_c ? "watching\n" : "ack\n");
    if ( !wat_c ) _quit(0);
  }
  else {
    //  error is [term tang]
    printf("nack\n");
    if ( mesa_is_cell(error) ) {
      for ( mesa_noun t = mesa_tail(error); mesa_is_cell(t); t = mesa_tail(t) ) {
        printf("  ");
        _tank(mesa_head(t), 0);
        printf("\n");
      }
    }
    _quit(1);
  }
  fflush(stdout);
}

static void
_boon(void* ctx, mesa_ship who, uint64_t bone, mesa_noun boon)
{
  mesa_noun mar, nou;

  switch ( mesa_gall_response(boon, &mar, &nou) ) {
    case MESA_GALL_FACT: {
      char* m = mesa_atom_get_cord(mar);
      printf("fact %%%s: ", m);
      free(m);
      _print("", nou);
    } break;
    case MESA_GALL_KICK: printf("kicked\n"); _quit(0); break;
    default: printf("strange response\n");
  }
  fflush(stdout);
}

static void
_sage(void* ctx, mesa_ship who, const char* path, mesa_noun gage)
{
  if ( MESA_NONE == gage ) {
    printf("no answer\n");
    _quit(1);
    return;
  }
  {
    char* m = mesa_atom_get_cord(mesa_head(gage));
    printf("%%%s: ", m);
    free(m);
  }
  _print("", mesa_tail(gage));
  fflush(stdout);
  _quit(0);
}

static mesa_plea_result
_plea(void* ctx, mesa_ship who, uint64_t bone, mesa_noun vane,
      mesa_noun path, mesa_noun payload, mesa_noun* error)
{
  mesa_gall_request req;

  //  ack gall's own flows; refuse anything else
  //
  if ( MESA_GALL_SYSTEM == mesa_gall_request_parse(vane, path, payload, &req) ) {
    return MESA_ACK;
  }
  return MESA_NACK;
}

/* comets need a star that's online to sponsor them
*/
static int
_mine(mesa_ident* ide)
{
  size_t    siz;
  uint8_t*  buf = mesa_host_get("https://bootstrap.urbit.org/comet-stars.jam", &siz);
  mesa_noun lis;
  mesa_ship sta[4096];
  size_t    len = 0;
  uint8_t   eny[64];

  if ( !buf ) {
    fprintf(stderr, "can't download comet-stars.jam\n");
    return 0;
  }
  lis = mesa_cue_bytes(siz, buf);
  free(buf);
  if ( MESA_NONE == lis ) return 0;
  for ( mesa_noun i = lis; mesa_is_cell(i) && len < 4096; i = mesa_tail(i) ) {
    uint64_t s;
    if ( mesa_atom_get_u64(mesa_head(i), &s) ) {
      sta[len++] = (mesa_ship){ .lo = s, .hi = 0 };
    }
  }
  mesa_unref(lis);

  if ( 64 != getrandom(eny, 64, 0) ) return 0;
  fprintf(stderr, "mining a comet under %zu stars...\n", len);
  return mesa_comet_mine(len, sta, eny, UINT64_MAX, ide);
}

static void
_usage(void)
{
  fprintf(stderr, "usage: mesa-ping (-F ~ship | -k keyfile | -c) [-s db] [-b] "
                  "[-w /path | -x /path] [-p port] ~ship agent [text]\n");
  exit(2);
}

int
main(int argc, char* argv[])
{
  mesa_host_config cfg = {0};
  mesa_ident       ide = {0};
  const char*      db  = 0;
  const char*      scr = 0;
  const char*      age = 0;
  const char*      txt = "hi";
  int              hav = 0, pos = 0;
  int              err;

  for ( int i = 1; i < argc; i++ ) {
    char* a = argv[i];
    if      ( !strcmp(a, "-F") && i + 1 < argc ) {
      mesa_ship who;
      if ( !mesa_ship_parse(argv[++i], &who) ) _usage();
      mesa_ident_fake(who, &ide);
      cfg.fake = true;
      cfg.engine.breached = true;
      hav = 1;
    }
    else if ( !strcmp(a, "-k") && i + 1 < argc ) {
      FILE* f = fopen(argv[++i], "r");
      char  key[8192] = {0};
      if ( !f || !fread(key, 1, sizeof(key) - 1, f) ) { perror("keyfile"); return 2; }
      fclose(f);
      if ( mesa_keyfile_parse(key, &ide, 1) < 1 ) { fprintf(stderr, "bad keyfile\n"); return 2; }
      hav = 1;
    }
    else if ( !strcmp(a, "-c") ) { hav = 2; }
    else if ( !strcmp(a, "-s") && i + 1 < argc ) { db = argv[++i]; }
    else if ( !strcmp(a, "-b") ) { cfg.engine.breached = true; }
    else if ( !strcmp(a, "-w") && i + 1 < argc ) { wat_c = argv[++i]; }
    else if ( !strcmp(a, "-x") && i + 1 < argc ) { scr = argv[++i]; }
    else if ( !strcmp(a, "-p") && i + 1 < argc ) { cfg.port = (uint16_t)atoi(argv[++i]); }
    else if ( !strcmp(a, "-P") && i + 1 < argc ) {
      mesa_ship  who;
      mesa_point pot;
      if ( !mesa_ship_parse(argv[++i], &who) ) _usage();
      curl_global_init(CURL_GLOBAL_DEFAULT);
      switch ( mesa_azimuth_fetch(0, who, &pot) ) {
        case MESA_AZIMUTH_OK: {
          char* spo = mesa_ship_render(pot.sponsor);
          printf("rift %llu, life %llu, sponsor %s%s\npass ",
                 (unsigned long long)pot.rift, (unsigned long long)pot.life,
                 spo, pot.has_sponsor ? "" : " (escaped)");
          for ( int j = 64; j >= 0; j-- ) printf("%02x", pot.pass[j]);
          printf("\n");
          free(spo);
          return 0;
        }
        case MESA_AZIMUTH_NOT_FOUND: printf("not spawned\n"); return 1;
        default: printf("lookup failed\n"); return 1;
      }
    }
    else if ( a[0] == '-' ) { _usage(); }
    else if ( pos == 0 ) { if ( !mesa_ship_parse(a, &her) ) _usage(); pos++; }
    else if ( pos == 1 ) { age = a; pos++; }
    else if ( pos == 2 ) { txt = a; pos++; }
  }
  if ( !hav || pos < (scr ? 1 : 2) ) _usage();

  curl_global_init(CURL_GLOBAL_DEFAULT);

  if ( 2 == hav ) {
    //  a comet's keys live in its state: reuse them if we have them
    //
    char  kef[1024];
    FILE* f;
    snprintf(kef, sizeof(kef), "%s.key", db ? db : "comet");
    if ( (f = fopen(kef, "r")) ) {
      char key[8192] = {0};
      size_t n = fread(key, 1, sizeof(key) - 1, f);
      fclose(f);
      if ( !n || mesa_keyfile_parse(key, &ide, 1) < 1 ) { fprintf(stderr, "bad %s\n", kef); return 2; }
    }
    else {
      char* str;
      if ( !_mine(&ide) ) { fprintf(stderr, "mining failed\n"); return 2; }
      str = mesa_keyfile_render(&ide);
      if ( (f = fopen(kef, "w")) ) { fprintf(f, "%s\n", str); fclose(f); }
      free(str);
    }
    {
      char* who = mesa_ship_render(ide.who);
      fprintf(stderr, "we are %s\n", who);
      free(who);
    }
  }

  if ( db ) {
    const char* e;
    if ( !(cfg.engine.store = mesa_store_sqlite(db, &e)) ) {
      fprintf(stderr, "%s: %s\n", db, e);
      return 2;
    }
  }
  else {
    cfg.engine.store = mesa_store_memory();
    cfg.engine.breached = true;
  }

  cfg.engine.ident = ide;
  cfg.engine.hooks = (mesa_hooks){ .done = _done, .boon = _boon, .sage = _sage,
                                   .plea = _plea };

  if ( MESA_OK != (err = mesa_host_open(&cfg, &hos)) ) {
    fprintf(stderr, "can't start: %d\n", err);
    return 2;
  }

  {
    mesa_engine* eng = mesa_host_engine(hos);

    if ( scr ) {
      mesa_keen(eng, her, scr);
    }
    else {
      mesa_flow_open(eng, her, &bon_d);
      if ( wat_c ) {
        mesa_noun pax = mesa_atom_u64(0);
        //  build the path back to front
        char* dup = strdup(wat_c);
        char* seg[64]; int n = 0;
        for ( char* t = strtok(dup, "/"); t && n < 64; t = strtok(0, "/") ) seg[n++] = t;
        while ( n-- ) pax = mesa_cell(mesa_atom_cord(seg[n]), pax);
        free(dup);
        mesa_gall_watch(eng, her, bon_d, age, pax);
      }
      else {
        mesa_gall_poke(eng, her, bon_d, age, "noun", mesa_atom_cord(txt));
      }
    }
    mesa_host_kick(hos);
  }

  mesa_host_run(hos);
  mesa_host_close(hos);
  mesa_store_free(cfg.engine.store);
  return ret_i;
}

/// @file
///
/// mesa-ping: talk to a ship's agents from the command line.
///
///   mesa-ping [identity] [options] ~ship agent [text]
///
/// identity and state: see cli.h (-F ~ship, -k keyfile, -c, -s db, -b, -p)
///
/// options:
///   -w /path        watch /path on the agent and print facts
///   -x /path        remote scry /path (e.g. /g/x/1/agent//1/foo)
///   -P ~ship        print ~ship's azimuth point and exit
///   -l              listen: stay online, ack every poke and print it
///                   (try `|hi ~our` from a ship's dojo)
///
/// Without -w or -x, pokes [agent] with [%noun text] and waits for the ack.

#include "mesa/engine.h"
#include "mesa/gall.h"
#include "mesa/host.h"
#include "mesa/ident.h"

#include "cli.h"

#include <curl/curl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <uv.h>

static mesa_host* hos;
static mesa_ship  her;
static uint64_t   bon_d;
static int        ret_i = 1;
static const char* wat_c;
static int         lis_i;

static void
_quit(int code)
{
  ret_i = code;
  mesa_host_stop(hos);
}

/* _noun(): print a noun: text atoms as 'cords', others as numbers
*/
static void
_noun(mesa_noun som, int dep, int tel)
{
  if ( dep > 64 ) { printf("..."); return; }
  if ( mesa_is_atom(som) ) {
    size_t   len = mesa_met3(som);
    uint64_t val;
    //  one-byte atoms are more likely numbers than text
    //
    if ( (len > 1) && (len < 4096) ) {
      char* str = mesa_atom_get_cord(som);
      int   txt = (strlen(str) == len);
      for ( char* c = str; *c && txt; c++ ) {
        if ( ((unsigned char)*c < 32) && (*c != '\n') ) txt = 0;
      }
      if ( txt ) { printf("'%s'", str); free(str); return; }
      free(str);
    }
    if ( mesa_atom_get_u64(som, &val) ) printf("%llu", (unsigned long long)val);
    else {
      uint8_t* buf = malloc(len);
      mesa_atom_get_bytes(som, len, buf);
      printf("0x");
      for ( size_t i = len; i--; ) printf("%02x", buf[i]);
      free(buf);
    }
    return;
  }
  if ( !tel ) printf("[");
  _noun(mesa_head(som), dep + 1, 0);
  printf(" ");
  _noun(mesa_tail(som), dep + 1, mesa_is_cell(mesa_tail(som)));
  if ( !tel ) printf("]");
}

static void
_print(const char* pre, mesa_noun som)
{
  printf("%s", pre);
  _noun(som, 0, 0);
  printf("\n");
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
    if ( !wat_c && !lis_i ) _quit(0);
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

  //  ack gall's own flows; when listening, ack and print pokes too
  //
  switch ( mesa_gall_request_parse(vane, path, payload, &req) ) {
    case MESA_GALL_SYSTEM: return MESA_ACK;
    case MESA_GALL_POKE: {
      char* src = mesa_ship_render(who);
      char* mar = mesa_atom_get_cord(req.mark);
      int   ack = lis_i;
      printf("%s: poke %%%s %%%s ", src, req.agent, mar);
      _print("", req.noun);
      if ( !ack ) printf("  (refused: not listening)\n");
      fflush(stdout);
      free(src);
      free(mar);
      return ack ? MESA_ACK : MESA_NACK;
    }
    default: return MESA_NACK;
  }
}

static void
_usage(void)
{
  fprintf(stderr, "usage: mesa-ping " CLI_USAGE " [-w /path | -x /path] [-l]\n"
                  "                 [~ship agent [text]]\n");
  exit(2);
}

int
main(int argc, char* argv[])
{
  cli         cli_u = {0};
  const char* scr = 0;
  const char* age = 0;
  const char* txt = "hi";
  int         pos = 0;
  int         err;

  curl_global_init(CURL_GLOBAL_DEFAULT);

  for ( int i = 1; i < argc; i++ ) {
    char* a = argv[i];
    if      ( cli_option(&cli_u, argc, argv, &i) ) {}
    else if ( !strcmp(a, "-w") && i + 1 < argc ) { wat_c = argv[++i]; }
    else if ( !strcmp(a, "-x") && i + 1 < argc ) { scr = argv[++i]; }
    else if ( !strcmp(a, "-l") ) { lis_i = 1; }
    else if ( !strcmp(a, "-P") && i + 1 < argc ) {
      mesa_ship  who;
      mesa_point pot;
      if ( !mesa_ship_parse(argv[++i], &who) ) _usage();
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
  if ( !cli_u.hav || pos < (lis_i ? 0 : scr ? 1 : 2) ) _usage();
  if ( !cli_open(&cli_u) ) return 2;

  cli_u.cfg.engine.hooks = (mesa_hooks){ .done = _done, .boon = _boon,
                                         .sage = _sage, .plea = _plea };
  if ( MESA_OK != (err = mesa_host_open(&cli_u.cfg, &hos)) ) {
    fprintf(stderr, "can't start: %d\n", err);
    return 2;
  }

  if ( lis_i && !pos ) {
    char* who = mesa_ship_render(cli_u.ide.who);
    printf("listening as %s\n", who);
    fflush(stdout);
    free(who);
  }
  else {
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
  mesa_store_free(cli_u.cfg.engine.store);
  return ret_i;
}

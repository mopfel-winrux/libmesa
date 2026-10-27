/// @file
///
/// mesa-bench: libmesa talking to libmesa over UDP on localhost.
///
///   mesa-bench [-s dir] [-q] [phase ...]
///
///   -s dir   keep both ships' state in SQLite databases in [dir]
///            (durable commits) instead of in memory
///   -q       quick: smaller runs
///   phase    run only these (after first contact): latency, pipe1,
///            pipe64, big, huge, scry-small, scry-1m, scry-32m
///
/// Forks a server (fake galaxy ~nus, 211) and runs a client (fake
/// galaxy ~dev, 210) against it, each a mesa_host in its own process.
/// The server acks every poke and serves blobs from its namespace.
/// Measures: first contact, poke round trips, poke throughput (one flow
/// and many), large pokes, and remote scry, small and large.

#include "mesa/engine.h"
#include "mesa/gall.h"
#include "mesa/host.h"
#include "mesa/ident.h"

#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#ifdef __linux__
#include <sys/prctl.h>
#endif
#include <unistd.h>
#include <uv.h>

#define CLI_GAL 210
#define SRV_GAL 211

static mesa_host*   hos;
static mesa_engine* eng;
static mesa_ship    her;
static int          qik_i;

static uint64_t
_now_ns(void)
{
  return uv_hrtime();
}

/* _blob(): [len] deterministic bytes as an atom (high byte nonzero)
*/
static mesa_noun
_blob(size_t len)
{
  uint8_t* buf = malloc(len);
  uint32_t x = 2463534242u;

  for ( size_t i = 0; i < len; i++ ) {
    x ^= x << 13; x ^= x >> 17; x ^= x << 5;
    buf[i] = (uint8_t)x;
  }
  buf[len - 1] |= 1;
  mesa_noun som = mesa_atom_bytes(len, buf);
  free(buf);
  return som;
}

/* server
*/
static mesa_plea_result
_srv_plea(void* ctx, mesa_ship who, uint64_t bone, mesa_noun vane,
          mesa_noun path, mesa_noun payload, mesa_noun* error)
{
  mesa_gall_request req;
  return ( MESA_GALL_POKE == mesa_gall_request_parse(vane, path, payload, &req) )
         ? MESA_ACK : MESA_DROP;
}

/* _srv_peek(): /g/x/0/bench//blob/<bytes> and /g/x/0/bench//small/<n>
*/
static mesa_noun
_srv_peek(void* ctx, const mesa_ship* who, const char* path)
{
  const char* pre = "/g/x/0/bench//";
  const char* rest;
  size_t      len;

  if ( strncmp(path, pre, strlen(pre)) ) return MESA_NONE;
  rest = path + strlen(pre);
  if ( !strncmp(rest, "blob/", 5) ) {
    len = strtoull(rest + 5, 0, 10);
    if ( !len || len > (1u << 30) ) return MESA_NONE;
    return mesa_cell(mesa_atom_cord("noun"), _blob(len));
  }
  if ( !strncmp(rest, "small/", 6) ) {
    return mesa_cell(mesa_atom_cord("noun"), mesa_atom_cord(rest + 6));
  }
  return MESA_NONE;
}

/* client: a sequence of phases, each started by _next()
*/
typedef struct {
  const char* nam;
  void      (*sta)(void);
} phase;

static int       pha_i = -1;
static uint64_t  t0_d;          //  phase start (ns)
static uint64_t  out_d;         //  outstanding operations
static uint64_t  tot_d;         //  operations in this phase
static uint64_t  siz_d;         //  bytes per operation
static uint64_t* rtt_d;         //  round trips (ns), latency phase
static uint64_t  rtn_d;
static uint64_t  sen_d;         //  when the in-flight poke was sent
static uint64_t  bon_d;         //  the latency/big flow

static void _next(void);

static void
_poke(uint64_t bone, mesa_noun som)
{
  int e = mesa_gall_poke(eng, her, bone, "bench", "noun", som);
  if ( MESA_OK != e ) {
    fprintf(stderr, "bench: poke failed: %d\n", e);
    exit(1);
  }
}

static int
_cmp(const void* a, const void* b)
{
  uint64_t x = *(const uint64_t*)a, y = *(const uint64_t*)b;
  return (x > y) - (x < y);
}

static double
_ms(uint64_t ns)
{
  return ns / 1e6;
}

static void
_report(void)
{
  uint64_t ns = _now_ns() - t0_d;
  double   sec = ns / 1e9;

  if ( siz_d ) {
    printf("  %-34s %8.1f ms   %8.1f MB/s   (%llu x %llu bytes)\n",
           "", _ms(ns), (double)(tot_d * siz_d) / sec / 1e6,
           (unsigned long long)tot_d, (unsigned long long)siz_d);
  }
  else {
    printf("  %-34s %8.1f ms   %8.0f /s     (%llu)\n", "", _ms(ns),
           tot_d / sec, (unsigned long long)tot_d);
  }
}

static void
_title(const char* t)
{
  printf("%s\n", t);
  fflush(stdout);
}

/* phases
*/
static void
_p_contact(void)
{
  _title("first contact (keys, then one poke)");
  mesa_flow_open(eng, her, &bon_d);
  tot_d = out_d = 1; siz_d = 0;
  _poke(bon_d, mesa_atom_cord("hi"));
}

static void
_p_latency(void)
{
  _title("poke round trip, one at a time");
  tot_d = out_d = qik_i ? 500 : 5000;
  siz_d = 0;
  rtt_d = calloc(tot_d, sizeof(uint64_t));
  rtn_d = 0;
  sen_d = _now_ns();
  _poke(bon_d, mesa_atom_u64(0));
}

static void
_flows(uint64_t fls, uint64_t per, size_t len)
{
  tot_d = out_d = fls * per;
  siz_d = (len > 8) ? len : 0;
  mesa_txn_begin(eng);
  for ( uint64_t f = 0; f < fls; f++ ) {
    uint64_t bone;
    mesa_flow_open(eng, her, &bone);
    for ( uint64_t i = 0; i < per; i++ ) {
      _poke(bone, (len > 8) ? _blob(len) : mesa_atom_u64(i));
    }
  }
  mesa_txn_commit(eng);
}

static void
_p_pipe1(void)
{
  _title("pokes queued on one flow");
  _flows(1, qik_i ? 1000 : 10000, 0);
}

static void
_p_pipe64(void)
{
  _title("pokes queued on 64 flows");
  _flows(64, qik_i ? 16 : 160, 0);
}

static void
_p_big(void)
{
  _title("1 MB pokes, one flow");
  _flows(1, qik_i ? 4 : 32, 1 << 20);
}

static void
_p_huge(void)
{
  _title("16 MB poke");
  _flows(1, qik_i ? 1 : 4, 16 << 20);
}

static char*
_blob_path(size_t len, uint64_t n)
{
  //  the client asks for /blob/<len>/<n> to defeat caches between
  //  runs; strtoull stops at the second slash
  static char pax[128];
  snprintf(pax, sizeof(pax), "/g/x/0/bench//blob/%zu/%llu", len,
           (unsigned long long)n);
  return pax;
}

static void
_keens(uint64_t n, size_t len)
{
  static uint64_t run_d;
  tot_d = out_d = n;
  siz_d = len;
  mesa_txn_begin(eng);
  for ( uint64_t i = 0; i < n; i++ ) {
    char pax[128];
    if ( len ) mesa_keen(eng, her, _blob_path(len, run_d++));
    else {
      snprintf(pax, sizeof(pax), "/g/x/0/bench//small/%llu",
               (unsigned long long)run_d++);
      mesa_keen(eng, her, pax);
    }
  }
  mesa_txn_commit(eng);
}

static void
_p_scry_small(void)
{
  _title("remote scry, small values, all at once");
  _keens(qik_i ? 500 : 5000, 0);
}

static void
_p_scry_1m(void)
{
  _title("remote scry, 1 MB, 8 at once");
  _keens(qik_i ? 2 : 8, 1 << 20);
}

static void
_p_scry_32m(void)
{
  _title("remote scry, 32 MB");
  _keens(1, 32 << 20);
}

static const phase pha_u[] = {
  { "contact",    _p_contact },
  { "latency",    _p_latency },
  { "pipe1",      _p_pipe1 },
  { "pipe64",     _p_pipe64 },
  { "big",        _p_big },
  { "huge",       _p_huge },
  { "scry-small", _p_scry_small },
  { "scry-1m",    _p_scry_1m },
  { "scry-32m",   _p_scry_32m },
};

static uv_idle_t nex_u;
static char**    pic_c;         //  phases picked, 0-terminated
static int       pin_i;

static int
_picked(int i)
{
  if ( !i || !pin_i ) return 1;
  for ( int j = 0; j < pin_i; j++ ) {
    if ( !strcmp(pic_c[j], pha_u[i].nam) ) return 1;
  }
  return 0;
}

static void
_on_next(uv_idle_t* idl_u)
{
  uv_idle_stop(idl_u);
  do pha_i++;
  while ( pha_i < (int)(sizeof(pha_u) / sizeof(pha_u[0])) && !_picked(pha_i) );
  if ( pha_i >= (int)(sizeof(pha_u) / sizeof(pha_u[0])) ) {
    mesa_host_stop(hos);
    return;
  }
  t0_d = _now_ns();
  pha_u[pha_i].sta();
  mesa_host_kick(hos);
}

/* _next(): start the next phase from the loop, not inside a hook
*/
static void
_next(void)
{
  uv_idle_start(&nex_u, _on_next);
}

static void
_one_done(void)
{
  if ( 0 == --out_d ) {
    if ( rtt_d ) {
      qsort(rtt_d, rtn_d, sizeof(uint64_t), _cmp);
      uint64_t sum = 0;
      for ( uint64_t i = 0; i < rtn_d; i++ ) sum += rtt_d[i];
      _report();
      printf("  %-34s p50 %.3f ms  p90 %.3f ms  p99 %.3f ms  max %.3f ms  mean %.3f ms\n",
             "", _ms(rtt_d[rtn_d / 2]), _ms(rtt_d[rtn_d * 9 / 10]),
             _ms(rtt_d[rtn_d * 99 / 100]), _ms(rtt_d[rtn_d - 1]),
             _ms(sum / rtn_d));
      free(rtt_d);
      rtt_d = 0;
    }
    else _report();
    fflush(stdout);
    _next();
  }
}

static void
_cli_done(void* ctx, mesa_ship who, uint64_t bone, mesa_noun error)
{
  if ( MESA_NONE != error ) {
    fprintf(stderr, "bench: nack\n");
    exit(1);
  }
  if ( rtt_d ) {
    uint64_t now = _now_ns();
    rtt_d[rtn_d++] = now - sen_d;
    if ( out_d > 1 ) {
      sen_d = now;
      _poke(bon_d, mesa_atom_u64(rtn_d));
    }
  }
  _one_done();
}

static void
_cli_sage(void* ctx, mesa_ship who, const char* path, mesa_noun gage)
{
  if ( MESA_NONE == gage || !mesa_is_cell(gage) ) {
    fprintf(stderr, "bench: no answer for %s\n", path);
    exit(1);
  }
  if ( siz_d && mesa_met3(mesa_tail(gage)) != siz_d ) {
    fprintf(stderr, "bench: %s: %zu bytes, wanted %llu\n", path,
            mesa_met3(mesa_tail(gage)), (unsigned long long)siz_d);
    exit(1);
  }
  _one_done();
}

static mesa_store*
_store(const char* dir, const char* nam, bool* fresh)
{
  if ( !dir ) {
    *fresh = true;
    return mesa_store_memory();
  }
  char        pax[4096];
  const char* err;
  mesa_store* sto;
  snprintf(pax, sizeof(pax), "%s/%s.db", dir, nam);
  unlink(pax);
  if ( !(sto = mesa_store_sqlite(pax, &err)) ) {
    fprintf(stderr, "bench: %s: %s\n", pax, err);
    exit(2);
  }
  *fresh = true;
  return sto;
}

/* _open(): a fake galaxy on its well-known localhost port
*/
static void
_open(uint8_t gal, const char* dir, mesa_hooks hok)
{
  mesa_host_config cfg = {0};
  bool             fre;
  int              e;

  mesa_ident_fake((mesa_ship){ .lo = gal }, &cfg.engine.ident);
  cfg.engine.store    = _store(dir, (gal == CLI_GAL) ? "client" : "server", &fre);
  cfg.engine.breached = fre;
  cfg.engine.hooks    = hok;
  cfg.fake            = true;
  if ( MESA_OK != (e = mesa_host_open(&cfg, &hos)) ) {
    fprintf(stderr, "bench: can't start galaxy %u: %d\n", gal, e);
    exit(2);
  }
  eng = mesa_host_engine(hos);
}

int
main(int argc, char* argv[])
{
  const char* dir = 0;
  int         pip[2];
  pid_t       pid;
  char        c;

  for ( int i = 1; i < argc; i++ ) {
    if      ( !strcmp(argv[i], "-s") && i + 1 < argc ) dir = argv[++i];
    else if ( !strcmp(argv[i], "-q") ) qik_i = 1;
    else if ( argv[i][0] != '-' ) {
      int ok = 0;
      for ( size_t j = 1; j < sizeof(pha_u) / sizeof(pha_u[0]); j++ ) {
        ok |= !strcmp(argv[i], pha_u[j].nam);
      }
      if ( !ok ) {
        fprintf(stderr, "mesa-bench: no phase %s\n", argv[i]);
        return 2;
      }
      if ( !pic_c ) pic_c = calloc(argc, sizeof(char*));
      pic_c[pin_i++] = argv[i];
    }
    else {
      fprintf(stderr, "usage: mesa-bench [-s dir] [-q] [phase ...]\n");
      return 2;
    }
  }
  printf("mesa-bench: two processes over UDP on 127.0.0.1, %s store\n\n",
         dir ? "SQLite" : "in-memory");
  fflush(stdout);

  //  the server tells us when its socket is bound
  if ( pipe(pip) ) return 2;
  if ( 0 == (pid = fork()) ) {
#ifdef __linux__
    prctl(PR_SET_PDEATHSIG, SIGTERM);
#endif
    close(pip[0]);
    _open(SRV_GAL, dir, (mesa_hooks){ .plea = _srv_plea, .peek = _srv_peek });
    if ( 1 != write(pip[1], "k", 1) ) _exit(2);
    close(pip[1]);
    mesa_host_run(hos);
    _exit(0);
  }
  close(pip[1]);
  if ( 1 != read(pip[0], &c, 1) ) {
    fprintf(stderr, "bench: server didn't start\n");
    return 2;
  }
  close(pip[0]);

  her = (mesa_ship){ .lo = SRV_GAL };
  _open(CLI_GAL, dir, (mesa_hooks){ .done = _cli_done, .sage = _cli_sage });
  uv_idle_init(mesa_host_loop(hos), &nex_u);
  _next();
  mesa_host_run(hos);

  kill(pid, SIGTERM);
  waitpid(pid, 0, 0);
  return 0;
}

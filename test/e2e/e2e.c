/// @file
///
/// Interop test against a real Arvo: a libmesa fake ~nec talks to a fake
/// ~zod (`urbit -F zod`) running /app/mesa-test.hoon from this directory,
/// over UDP on localhost (fake galaxy ~g listens on 31337+g).
///
///   mesa-e2e [-s state.db] [-i ~galaxy | -c]
///
/// Runs a script of pokes, a watch, remote scry and a cork, checking each
/// response, and exits nonzero on failure.

#include "c3/c3.h"
#include "mesa/engine.h"
#include "mesa/ident.h"
#include "mesa/noun.h"

#include <arpa/inet.h>
#include <errno.h>
#include <netinet/in.h>
#include <poll.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <unistd.h>

static int          sok_i;
static mesa_engine* eng;
static mesa_ship    zod, nec;
static int          fail_i;

static uint64_t
_now(void)
{
  struct timeval tv;
  gettimeofday(&tv, 0);
  return ((uint64_t)tv.tv_sec * 1000) + (tv.tv_usec / 1000);
}

static void
_log(const char* fmt, ...)
{
  va_list ap;
  va_start(ap, fmt);
  fprintf(stderr, "e2e: ");
  vfprintf(stderr, fmt, ap);
  fprintf(stderr, "\n");
  va_end(ap);
}

/* events the script waits for
*/
typedef struct _ev {
  char         kin_c[16];
  uint64_t     bon_d;
  mesa_noun    som;
  struct _ev*  nex_u;
} _ev;

static _ev* evs_u;

static void
_push(const char* kin_c, uint64_t bon_d, mesa_noun som)
{
  _ev*  ev  = calloc(1, sizeof(*ev));
  _ev** end = &evs_u;

  snprintf(ev->kin_c, sizeof(ev->kin_c), "%s", kin_c);
  ev->bon_d = bon_d;
  ev->som   = ( MESA_NONE == som ) ? MESA_NONE : mesa_ref(som);
  while ( *end ) end = &(*end)->nex_u;
  *end = ev;
}

/* hooks
*/
static void
_send(void* ctx, mesa_lane lane, const uint8_t* buf, size_t len)
{
  struct sockaddr_in add = { .sin_family = AF_INET };

  add.sin_addr.s_addr = htonl(0x7f000001);
  if ( MESA_LANE_GALAXY == lane.kind ) {
    add.sin_port = htons(31337 + lane.galaxy);
  }
  else {
    add.sin_port = htons(lane.port);
  }
  sendto(sok_i, buf, len, 0, (struct sockaddr*)&add, sizeof(add));
}

static mesa_ship need_u[16];
static int       nen_i;

static void
_need(void* ctx, mesa_ship who)
{
  if ( nen_i < 16 ) need_u[nen_i++] = who;
}

static mesa_plea_result
_plea(void* ctx, mesa_ship her, uint64_t bone, mesa_noun vane,
      mesa_noun path, mesa_noun payload, mesa_noun* error)
{
  char* van_c = mesa_atom_get_cord(vane);
  _log("plea from ~%s on bone %lu: vane %%%s", "zod", (unsigned long)bone, van_c);
  free(van_c);
  _push("plea", bone, payload);
  return MESA_ACK;
}

static void
_boon(void* ctx, mesa_ship her, uint64_t bone, mesa_noun payload)
{
  _push("boon", bone, payload);
}

static void
_done(void* ctx, mesa_ship her, uint64_t bone, mesa_noun error)
{
  _push(( MESA_NONE == error ) ? "ack" : "nack", bone, error);
}

static void
_corked(void* ctx, mesa_ship her, uint64_t bone)
{
  _push("corked", bone, MESA_NONE);
}

static void
_sage(void* ctx, mesa_ship her, const char* path, mesa_noun gage)
{
  _push("sage", 0, gage);
}

/* the loop
*/
static void
_answer_points(void)
{
  while ( nen_i ) {
    mesa_ship  who = need_u[--nen_i];
    mesa_ident ide;
    mesa_point pot = { .who = who, .rift = 0, .life = 1, .has_sponsor = true };

    mesa_ident_fake(who, &ide);
    mesa_ident_pass(&ide, pot.pass);
    pot.sponsor = mesa_ship_sein(who);
    mesa_engine_point(eng, &pot);
  }
}

static void
_spin(uint64_t ms)
{
  uint64_t end = _now() + ms;

  while ( _now() < end ) {
    struct pollfd pfd = { .fd = sok_i, .events = POLLIN };
    uint64_t      dea = mesa_engine_deadline(eng);
    uint64_t      now = _now();
    int           tim = (int)( (dea <= now) ? 0 : ((dea - now) > 50 ? 50 : (dea - now)) );

    _answer_points();

    if ( 0 < poll(&pfd, 1, tim) ) {
      uint8_t            buf[2048];
      struct sockaddr_in add;
      socklen_t          adl = sizeof(add);
      ssize_t            len = recvfrom(sok_i, buf, sizeof(buf), 0,
                                        (struct sockaddr*)&add, &adl);
      if ( len > 0 ) {
        mesa_lane lan = { .kind = MESA_LANE_IP,
                          .ip   = ntohl(add.sin_addr.s_addr),
                          .port = ntohs(add.sin_port) };
        mesa_engine_hear(eng, lan, buf, (size_t)len, _now());
      }
    }
    if ( mesa_engine_deadline(eng) <= _now() ) {
      mesa_engine_wake(eng, _now());
    }
  }
}

/* _wait(): wait up to [ms] for an event of [kin_c]; pop and return it
*/
static _ev*
_wait(const char* kin_c, uint64_t ms)
{
  uint64_t end = _now() + ms;

  while ( 1 ) {
    for ( _ev** ev = &evs_u; *ev; ev = &(*ev)->nex_u ) {
      if ( !strcmp((*ev)->kin_c, kin_c) ) {
        _ev* hit = *ev;
        *ev = hit->nex_u;
        return hit;
      }
    }
    if ( _now() >= end ) return 0;
    _spin(20);
  }
}

#define check(c, ...) do { if ( !(c) ) { _log("FAIL: " __VA_ARGS__); fail_i = 1; } \
                           else { _log("ok: " __VA_ARGS__); } } while (0)

static mesa_noun
_ge(const char* agent)
{
  return mesa_cell(mesa_atom_cord("ge"),
                   mesa_cell(mesa_atom_cord(agent), mesa_atom_u64(0)));
}

//  [%0 %m %noun noun]
//
static mesa_err
_poke(uint64_t bone, mesa_noun noun)
{
  return mesa_flow_plea(eng, zod, bone, mesa_atom_cord("g"), _ge("mesa-test"),
                        mesa_qual(mesa_atom_u64(0), mesa_atom_cord("m"),
                                  mesa_atom_cord("noun"), noun));
}

int
main(int argc, char* argv[])
{
  struct sockaddr_in add = { .sin_family = AF_INET };
  mesa_config        cfg = {0};
  const char*        db_c = 0;
  mesa_err           err;
  uint64_t           bon_d, sub_d;
  _ev*               ev;

  mesa_ship_parse("~zod", &zod);
  mesa_ship_parse("~nec", &nec);

  int com_i = 0;
  for ( int i = 1; i < argc; i++ ) {
    if ( !strcmp(argv[i], "-s") && (i + 1 < argc) ) db_c = argv[++i];
    if ( !strcmp(argv[i], "-i") && (i + 1 < argc) ) mesa_ship_parse(argv[++i], &nec);
    if ( !strcmp(argv[i], "-c") ) com_i = 1;
  }

  //  a comet under ~marzod, on an ephemeral port
  //
  if ( com_i ) {
    mesa_ship sta[1];
    uint8_t   eny[64];
    mesa_ship_parse("~marzod", &sta[0]);
    for ( int i = 0; i < 64; i++ ) eny[i] = (uint8_t)(_now() >> (i % 8)) ^ (uint8_t)i;
    if ( !mesa_comet_mine(1, sta, eny, UINT64_MAX, &cfg.ident) ) return 2;
    nec = cfg.ident.who;
    {
      char* who_c = mesa_ship_render(nec);
      _log("we are %s", who_c);
      free(who_c);
    }
  }

  sok_i = socket(AF_INET, SOCK_DGRAM, 0);
  add.sin_addr.s_addr = htonl(0x7f000001);
  add.sin_port = htons(com_i ? 0 : (31337 + nec.lo));
  if ( bind(sok_i, (struct sockaddr*)&add, sizeof(add)) ) {
    _log("bind %u: %s", 31337 + (unsigned)nec.lo, strerror(errno));
    return 2;
  }

  if ( !com_i ) {
    mesa_ident_fake(nec, &cfg.ident);
  }
  if ( db_c ) {
    const char* err_c;
    cfg.store = mesa_store_sqlite(db_c, &err_c);
    if ( !cfg.store ) { _log("store: %s", err_c); return 2; }
  }
  else {
    cfg.store = mesa_store_memory();
  }
  cfg.breached = true;
  cfg.no_ping  = true;   //  we are a galaxy anyway
  cfg.now      = _now();
  cfg.hooks    = (mesa_hooks){ .send = _send, .need_point = _need, .plea = _plea,
                               .boon = _boon, .done = _done, .corked = _corked,
                               .sage = _sage };

  if ( MESA_OK != (err = mesa_engine_open(&cfg, &eng)) ) {
    _log("open: %d", err);
    return 2;
  }

  //  1. a poke, acked
  //
  mesa_flow_open(eng, zod, &bon_d);
  _poke(bon_d, mesa_atom_cord("hello"));
  ev = _wait("ack", 60000);
  check(ev && ev->bon_d == bon_d, "poke %%hello acked");

  //  2. a crashing poke, nacked with a naxplanation
  //
  _poke(bon_d, mesa_atom_cord("crash"));
  ev = _wait("nack", 60000);
  check(ev != 0, "poke %%crash nacked");
  if ( ev && (MESA_NONE != ev->som) && mesa_is_cell(ev->som) ) {
    char* tag_c = mesa_atom_get_cord(mesa_head(ev->som));
    _log("    naxplanation tag %%%s", tag_c);
    free(tag_c);
  }

  //  3. the flow keeps going after a nack
  //
  _poke(bon_d, mesa_atom_cord("after"));
  ev = _wait("ack", 60000);
  check(ev != 0, "poke after nack acked");

  //  4. a watch: acked, then facts as boons [%d %noun ...]
  //
  mesa_flow_open(eng, zod, &sub_d);
  mesa_flow_plea(eng, zod, sub_d, mesa_atom_cord("g"), _ge("mesa-test"),
                 mesa_trel(mesa_atom_u64(0), mesa_atom_cord("s"),
                           mesa_cell(mesa_atom_cord("updates"), mesa_atom_u64(0))));
  ev = _wait("ack", 60000);
  check(ev && ev->bon_d == sub_d, "watch acked");
  ev = _wait("boon", 60000);
  check(ev && ev->bon_d == sub_d, "first fact");
  if ( ev ) {
    //  [%d %noun %hello ~nec]
    mesa_noun pay = ev->som;
    char*     d_c = mesa_atom_get_cord(mesa_head(pay));
    check(!strcmp(d_c, "d"), "fact is [%%d mark noun]");
    free(d_c);
  }

  _poke(bon_d, mesa_atom_cord("tick"));
  ev = _wait("ack", 60000);
  check(ev != 0, "poke %%tick acked");
  ev = _wait("boon", 60000);
  check(ev && ev->bon_d == sub_d, "fact for the poke");

  //  5. a big poke: multi-fragment both ways
  //
  {
    uint8_t* buf = malloc(6000);
    for ( int i = 0; i < 6000; i++ ) buf[i] = (uint8_t)('a' + (i % 26));
    _poke(bon_d, mesa_atom_bytes(6000, buf));
    free(buf);
    ev = _wait("ack", 60000);
    check(ev != 0, "6KB poke acked");
    ev = _wait("boon", 60000);
    check(ev && mesa_met3(mesa_at(31, ev->som)) == 6000, "6KB fact (%zu bytes)",
          ev ? mesa_met3(mesa_at(31, ev->som)) : 0);
  }

  //  6. ~zod pokes us back (and opens its flub flow)
  //
  _poke(bon_d, mesa_atom_cord("poke-back"));
  ev = _wait("ack", 60000);
  check(ev != 0, "poke %%poke-back acked");
  {
    int got_i = 0;
    for ( int i = 0; i < 3 && !got_i; i++ ) {
      ev = _wait("plea", 30000);
      if ( !ev ) break;
      //  [%0 %m %noun %back n]
      if ( mesa_is_cell(ev->som) && (MESA_NONE != mesa_at(14, ev->som)) ) {
        char* m_c = mesa_atom_get_cord(mesa_at(6, ev->som));
        got_i = !strcmp(m_c, "m");
        free(m_c);
      }
    }
    check(got_i, "~zod poked us");
  }

  //  7. remote scry of a grown path
  //
  mesa_keen(eng, zod, "/g/x/1/mesa-test//1/hello");
  ev = _wait("sage", 60000);
  check(ev && (MESA_NONE != ev->som), "remote scry");
  if ( ev && (MESA_NONE != ev->som) ) {
    mesa_noun wan = mesa_cell(mesa_atom_cord("noun"), mesa_atom_cord("world"));
    check(mesa_eq(ev->som, wan), "remote scry is [%%noun 'world']");
    mesa_unref(wan);
  }

  //  8. leave the subscription and cork its flow
  //
  mesa_flow_plea(eng, zod, sub_d, mesa_atom_cord("g"), _ge("mesa-test"),
                 mesa_trel(mesa_atom_u64(0), mesa_atom_cord("u"), mesa_atom_u64(0)));
  ev = _wait("ack", 60000);
  check(ev && ev->bon_d == sub_d, "leave acked");
  mesa_flow_cork(eng, zod, sub_d);
  ev = _wait("corked", 120000);
  check(ev && ev->bon_d == sub_d, "subscription flow corked");

  _spin(2000);
  mesa_engine_close(eng);
  mesa_store_free(cfg.store);
  _log(fail_i ? "FAILED" : "all ok");
  return fail_i;
}

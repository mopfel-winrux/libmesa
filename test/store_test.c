/// @file
///
/// Stores: key-value semantics for both backends, the SQLite lockfile,
/// boot checks, and exactly-once delivery into an application table
/// that shares the engine's SQLite transaction, across restarts.

#include "compat.h"
#include "mesa/engine.h"
#include "mesa/ident.h"
#include "mesa/store.h"
#include "sqlite3.h"

#include <sys/stat.h>
#include <unistd.h>

static c3_i ret_i = 0;

#define expect(c, ...) do { if ( !(c) ) { \
    fprintf(stderr, "store: %s:%d: ", __FILE__, __LINE__); \
    fprintf(stderr, __VA_ARGS__); fprintf(stderr, "\r\n"); ret_i = 1; } } while (0)

static c3_c dir_c[64];

static c3_c*
_path(const c3_c* nam_c)
{
  static c3_c pat_c[256];
  snprintf(pat_c, sizeof(pat_c), "%s/%s", dir_c, nam_c);
  return pat_c;
}

/* key-value semantics
*/
static int
_collect(void* arg, const uint8_t* key_y, size_t key_z,
         const uint8_t* val_y, size_t val_z)
{
  c3_c* out_c = arg;
  strncat(out_c, (const c3_c*)key_y, key_z);
  strcat(out_c, ",");
  return 0;
}

static void
_put(mesa_store* sto, const c3_c* key_c, const c3_c* val_c)
{
  sto->put(sto->ctx, (c3_y*)key_c, strlen(key_c), (c3_y*)val_c, strlen(val_c));
}

static c3_o
_has(mesa_store* sto, const c3_c* key_c, const c3_c* val_c)
{
  c3_y* val_y;
  c3_z  val_z;
  c3_i  r = sto->get(sto->ctx, (c3_y*)key_c, strlen(key_c), &val_y, &val_z);
  c3_o  ret_o;

  if ( r ) return ( val_c ? c3n : c3y );
  ret_o = __( val_c && (strlen(val_c) == val_z) && !memcmp(val_y, val_c, val_z) );
  c3_free(val_y);
  return ret_o;
}

static void
_test_kv(mesa_store* sto, const c3_c* nam_c)
{
  c3_c out_c[256];

  expect(0 == sto->begin(sto->ctx), "%s begin", nam_c);
  _put(sto, "b1", "x");
  _put(sto, "a1", "y");
  _put(sto, "b2", "z");
  _put(sto, "b\xff", "w");
  _put(sto, "c", "v");
  expect(c3y == _has(sto, "a1", "y"), "%s get", nam_c);
  _put(sto, "a1", "Y");
  expect(c3y == _has(sto, "a1", "Y"), "%s overwrite", nam_c);
  expect(0 == sto->commit(sto->ctx), "%s commit", nam_c);

  out_c[0] = 0;
  sto->scan(sto->ctx, (c3_y*)"b", 1, _collect, out_c);
  expect(!strcmp(out_c, "b1,b2,b\xff,"), "%s prefix scan: %s", nam_c, out_c);

  out_c[0] = 0;
  sto->scan(sto->ctx, (c3_y*)"b\xff", 2, _collect, out_c);
  expect(!strcmp(out_c, "b\xff,"), "%s 0xff prefix scan", nam_c);

  out_c[0] = 0;
  sto->scan(sto->ctx, (c3_y*)"", 0, _collect, out_c);
  expect(!strcmp(out_c, "a1,b1,b2,b\xff,c,"), "%s full scan: %s", nam_c, out_c);

  //  rollback
  //
  sto->begin(sto->ctx);
  _put(sto, "d", "1");
  sto->del(sto->ctx, (c3_y*)"a1", 2);
  sto->rollback(sto->ctx);
  expect(c3y == _has(sto, "d", 0), "%s rolled back put", nam_c);
  expect(c3y == _has(sto, "a1", "Y"), "%s rolled back del", nam_c);

  //  savepoints
  //
  sto->begin(sto->ctx);
  _put(sto, "e", "1");
  sto->mark(sto->ctx);
  _put(sto, "f", "1");
  _put(sto, "e", "2");
  sto->rewind(sto->ctx);
  sto->release(sto->ctx);
  _put(sto, "g", "1");
  sto->commit(sto->ctx);
  expect(c3y == _has(sto, "e", "1"), "%s savepoint kept", nam_c);
  expect(c3y == _has(sto, "f", 0), "%s savepoint undone", nam_c);
  expect(c3y == _has(sto, "g", "1"), "%s after savepoint", nam_c);
}

/* a two-node network over SQLite stores
*/
typedef struct _dgram {
  c3_w           tar_w;
  mesa_lane      fro_u;
  c3_z           len_z;
  c3_y*          buf_y;
  struct _dgram* nex_u;
} _dgram;

typedef struct {
  mesa_ident   ide;
  c3_c         pat_c[256];
  mesa_store*  sto;
  mesa_engine* eng;
  c3_w         idx_w;
  mesa_ship    ask_u[8];
  c3_w         asn_w;
  c3_w         don_w;     //  dones
  c3_w         nak_w;     //  naxplanations
} _node;

static _node   nod_u[2];
static _dgram* net_u;
static c3_d    now_d = 5000;

static void
_h_send(void* ctx, mesa_lane lane, const uint8_t* buf, size_t len)
{
  _node*   nod = ctx;
  _dgram*  dgm_u = c3_calloc(sizeof(*dgm_u));
  _dgram** end_u = &net_u;

  //  both are galaxies
  //
  if ( MESA_LANE_GALAXY != lane.kind ) {
    c3_free(dgm_u);
    return;
  }
  dgm_u->tar_w = ( lane.galaxy == nod_u[0].ide.who.lo ) ? 0 : 1;
  dgm_u->fro_u = (mesa_lane){ .kind = MESA_LANE_GALAXY,
                              .galaxy = (uint8_t)nod->ide.who.lo };
  dgm_u->len_z = len;
  dgm_u->buf_y = c3_malloc(len);
  memcpy(dgm_u->buf_y, buf, len);
  while ( *end_u ) end_u = &(*end_u)->nex_u;
  *end_u = dgm_u;
}

static void
_h_need(void* ctx, mesa_ship who)
{
  _node* nod = ctx;
  if ( nod->asn_w < 8 ) nod->ask_u[nod->asn_w++] = who;
}

//  the application: record each request in its own table, in the
//  engine's transaction; refuse multiples of 7 (and roll their row back)
//
static mesa_plea_result
_h_plea(void* ctx, mesa_ship her, uint64_t bone, mesa_noun vane,
        mesa_noun path, mesa_noun payload, mesa_noun* error)
{
  _node*        nod = ctx;
  sqlite3*      db  = mesa_store_sqlite_db(nod->sto);
  sqlite3_stmt* stm;
  c3_d          val_d = 0;
  c3_i          r;

  mesa_atom_get_u64(payload, &val_d);

  sqlite3_prepare_v2(db, "INSERT INTO seen (n) VALUES (?1)", -1, &stm, 0);
  sqlite3_bind_int64(stm, 1, (sqlite3_int64)val_d);
  r = sqlite3_step(stm);
  sqlite3_finalize(stm);

  //  a duplicate would violate the UNIQUE constraint
  //
  expect(SQLITE_DONE == r, "insert %" PRIu64 ": duplicate delivery", val_d);

  if ( 0 == (val_d % 7) ) {
    *error = mesa_cell(mesa_atom_cord("seven"), mesa_atom_u64(0));
    return MESA_NACK;
  }
  return MESA_ACK;
}

static void
_h_done(void* ctx, mesa_ship her, uint64_t bone, mesa_noun error)
{
  _node* nod = ctx;
  if ( MESA_NONE == error ) nod->don_w++;
  else                      nod->nak_w++;
}

static mesa_err
_open(_node* nod, bool bre)
{
  mesa_config cfg = {0};
  const c3_c* err_c;

  nod->sto = mesa_store_sqlite(nod->pat_c, &err_c);
  if ( !nod->sto ) {
    fprintf(stderr, "store: %s\r\n", err_c);
    return MESA_E_STORE;
  }
  sqlite3_exec(mesa_store_sqlite_db(nod->sto),
               "CREATE TABLE IF NOT EXISTS seen (n INTEGER UNIQUE)", 0, 0, 0);

  cfg.ident    = nod->ide;
  cfg.store    = nod->sto;
  cfg.breached = bre;
  cfg.no_ping  = true;
  cfg.now      = now_d;
  cfg.hooks    = (mesa_hooks){ .ctx = nod, .send = _h_send, .need_point = _h_need,
                               .plea = _h_plea, .done = _h_done };
  return mesa_engine_open(&cfg, &nod->eng);
}

static void
_close(_node* nod)
{
  mesa_engine_close(nod->eng);
  mesa_store_free(nod->sto);
  nod->eng = 0;
  nod->sto = 0;
}

static void
_run(c3_d max_d)
{
  c3_d end_d = now_d + max_d;

  while ( now_d < end_d ) {
    for ( c3_w i = 0; i < 2; i++ ) {
      while ( nod_u[i].asn_w ) {
        mesa_ship  who = nod_u[i].ask_u[--nod_u[i].asn_w];
        mesa_ident ide;
        mesa_point pot = { .who = who, .rift = 0, .life = 1 };
        mesa_ident_fake(who, &ide);
        mesa_ident_pass(&ide, pot.pass);
        mesa_engine_point(nod_u[i].eng, &pot);
      }
    }

    if ( net_u ) {
      _dgram* dgm_u = net_u;
      net_u = dgm_u->nex_u;
      if ( rand() % 100 >= 20 ) {
        mesa_engine_hear(nod_u[dgm_u->tar_w].eng, dgm_u->fro_u,
                         dgm_u->buf_y, dgm_u->len_z, now_d);
      }
      c3_free(dgm_u->buf_y);
      c3_free(dgm_u);
      continue;
    }

    {
      c3_d dea_d = c3_min(mesa_engine_deadline(nod_u[0].eng),
                          mesa_engine_deadline(nod_u[1].eng));
      if ( dea_d > end_d ) { now_d = end_d; break; }
      now_d = c3_max(now_d + 1, dea_d);
      mesa_engine_wake(nod_u[0].eng, now_d);
      mesa_engine_wake(nod_u[1].eng, now_d);
    }
  }
}

static void
_drop_net(void)
{
  while ( net_u ) {
    _dgram* nex_u = net_u->nex_u;
    c3_free(net_u->buf_y);
    c3_free(net_u);
    net_u = nex_u;
  }
}

static c3_w
_seen(_node* nod)
{
  sqlite3_stmt* stm;
  c3_w          n_w = 0;

  sqlite3_prepare_v2(mesa_store_sqlite_db(nod->sto),
                     "SELECT count(*) FROM seen", -1, &stm, 0);
  if ( SQLITE_ROW == sqlite3_step(stm) ) {
    n_w = (c3_w)sqlite3_column_int(stm, 0);
  }
  sqlite3_finalize(stm);
  return n_w;
}

static void
_test_exactly_once(void)
{
  _node* zod = &nod_u[0];
  _node* nec = &nod_u[1];
  c3_d   bon_d;
  c3_w   tot_w = 60;

  mesa_ship_parse("~zod", &zod->ide.who);
  mesa_ship_parse("~nec", &nec->ide.who);
  mesa_ident_fake(zod->ide.who, &zod->ide);
  mesa_ident_fake(nec->ide.who, &nec->ide);
  snprintf(zod->pat_c, sizeof(zod->pat_c), "%s", _path("zod.db"));
  snprintf(nec->pat_c, sizeof(nec->pat_c), "%s", _path("nec.db"));

  //  an empty store needs to be told it may start fresh
  //
  expect(MESA_E_FRESH == _open(zod, false), "refuse fresh state");
  _close(zod);
  expect(MESA_OK == _open(zod, true), "fresh with breached");
  expect(MESA_OK == _open(nec, true), "nec fresh");

  //  a second process can't take the same state
  //
  {
    const c3_c* err_c;
    mesa_store* two = mesa_store_sqlite(zod->pat_c, &err_c);
    expect(!two, "second open is locked out");
    if ( two ) mesa_store_free(two);
  }

  //  queue everything in one application transaction
  //
  expect(MESA_OK == mesa_txn_begin(zod->eng), "txn");
  mesa_flow_open(zod->eng, nec->ide.who, &bon_d);
  for ( c3_w i = 1; i <= tot_w; i++ ) {
    mesa_flow_plea(zod->eng, nec->ide.who, bon_d, mesa_atom_cord("g"),
                   mesa_cell(mesa_atom_cord("ge"),
                             mesa_cell(mesa_atom_cord("app"), mesa_atom_u64(0))),
                   mesa_atom_u64(i));
  }
  expect(MESA_OK == mesa_txn_commit(zod->eng), "commit");

  //  crash both sides over and over
  //
  for ( c3_w r = 0; r < 12; r++ ) {
    _run(2000 + (rand() % 3000));
    _drop_net();
    if ( r & 1 ) {
      c3_w don_w = zod->don_w, nak_w = zod->nak_w;
      _close(zod);
      expect(MESA_OK == _open(zod, false), "reopen zod");
      zod->don_w = don_w; zod->nak_w = nak_w;
    }
    else {
      _close(nec);
      expect(MESA_OK == _open(nec, false), "reopen nec");
    }
  }
  _run(60 * 60 * 1000);

  {
    c3_w sev_w = tot_w / 7;
    expect(_seen(nec) == tot_w - sev_w, "nec's table: %u rows, want %u",
           _seen(nec), tot_w - sev_w);
    expect(zod->don_w == tot_w - sev_w, "acks: %u", zod->don_w);
    expect(zod->nak_w == sev_w, "nacks: %u", zod->nak_w);
  }

  //  the store belongs to ~zod
  //
  _close(nec);
  {
    mesa_ident tmp = nec->ide;
    nec->ide = zod->ide;
    snprintf(nec->pat_c, sizeof(nec->pat_c), "%s", _path("nec.db"));
    expect(MESA_E_IDENTITY == _open(nec, true), "wrong ship");
    _close(nec);
    nec->ide = tmp;
  }
  _close(zod);
}

int
main(int argc, char* argv[])
{
  mesa_store* sto;
  const c3_c* err_c;

  snprintf(dir_c, sizeof(dir_c), "/tmp/mesa-store-XXXXXX");
  u3_assert( mkdtemp(dir_c) );
  srand(3);

  sto = mesa_store_memory();
  _test_kv(sto, "memory");
  mesa_store_free(sto);

  sto = mesa_store_sqlite(_path("kv.db"), &err_c);
  u3_assert( sto );
  _test_kv(sto, "sqlite");
  mesa_store_free(sto);

  _test_exactly_once();

  //  clean up
  //
  {
    c3_c cmd_c[128];
    snprintf(cmd_c, sizeof(cmd_c), "rm -rf %s", dir_c);
    if ( system(cmd_c) ) {}
  }

  if ( ret_i ) {
    fprintf(stderr, "store: failed\r\n");
  }
  else {
    fprintf(stderr, "store ok\r\n");
  }
  return ret_i;
}

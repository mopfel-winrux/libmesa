/// @file
///
/// Engine tests: Hoon vectors for namespace pages, then engines talking
/// over a simulated network.

#include "eng.h"
#include "mesa/engine.h"
#include "mesa/gall.h"
#include "mesa/ident.h"

static c3_i ret_i = 0;

#define expect(c, ...) do { if ( !(c) ) { \
    fprintf(stderr, "engine: %s:%d: ", __FILE__, __LINE__); \
    fprintf(stderr, __VA_ARGS__); fprintf(stderr, "\r\n"); ret_i = 1; } } while (0)

/* hex vectors (atoms, big-endian text) to little-endian bytes
*/
static c3_z
_hex(const c3_c* hex_c, c3_y* out_y, c3_z cap_z)
{
  c3_z len_z = strlen(hex_c);

  memset(out_y, 0, cap_z);
  for ( c3_z j_z = 0; j_z < len_z; j_z++ ) {
    c3_c c = hex_c[len_z - 1 - j_z];
    c3_y v = ( c <= '9' ) ? c - '0' : c - 'a' + 10;
    u3_assert( j_z / 2 < cap_z );
    out_y[j_z / 2] |= v << (4 * (j_z & 1));
  }
  return (len_z + 1) / 2;
}

static mesa_ship
_ship(const c3_c* str_c)
{
  mesa_ship who;
  u3_assert( mesa_ship_parse(str_c, &who) );
  return who;
}

static mesa_point
_fake_point(mesa_ship who)
{
  mesa_ident ide;
  mesa_point pot = {0};

  mesa_ident_fake(who, &ide);
  pot.who  = who;
  pot.rift = 0;
  pot.life = 1;
  mesa_ident_pass(&ide, pot.pass);
  pot.has_sponsor = true;
  pot.sponsor = mesa_ship_sein(who);
  return pot;
}

/* vectors from Hoon: ~nec's ack page for ~zod's first plea on bone 0
*/
static void
_test_vectors(void)
{
  mesa_config  cfg = {0};
  mesa_engine* eng;
  mesa_point   pot = _fake_point(_ship("~zod"));
  eng_peer*    per_u;
  eng_flow*    flo_u;
  eng_line*    lin_u;
  c3_c*        pat_c;
  c3_c*        inn_c;
  c3_y         wan_y[64];
  c3_z         wan_z;

  mesa_ident_fake(_ship("~nec"), &cfg.ident);
  cfg.store    = mesa_store_memory();
  cfg.breached = true;
  cfg.no_ping  = true;
  cfg.now      = 1000;
  u3_assert( MESA_OK == mesa_engine_open(&cfg, &eng) );

  mesa_engine_point(eng, &pot);
  per_u = eng_peer_get(eng, (u3_ship){ pot.who.lo, pot.who.hi });
  u3_assert( per_u && (c3y == per_u->kno_o) );

  wan_z = _hex("d7324754895f7e31840cea6d8d5a1901e22eb93f80c12d462172d34a7971512",
               wan_y, 32);
  expect(!memcmp(wan_y, per_u->sym_y, 32), "symmetric key");

  inn_c = eng_flow_path(0, "ack", DIRE_BAK, per_u->who_u, 1);
  expect(!strcmp(inn_c, "/a/x/1//flow/0/ack/bak/~zod/1"), "flow path %s", inn_c);

  pat_c = eng_chum_path(eng, per_u, c3y, inn_c);
  expect(!strcmp(pat_c, "chum/1/~zod/1/0v1u5.5v5vr.j8l61.5msj0.e1nbm.oi7eo."
                        "s7sv0.ccgjf.9gmr8.860g5.g6l1b.j53hp.mv1lj.51g9h.uaqpf"),
         "chum path %s", pat_c);

  //  round trip the sealed path
  //
  {
    c3_c* opn_c = eng_open_chum(eng, per_u, pat_c);
    expect(opn_c && !strcmp(opn_c, inn_c), "open chum");
    c3_free(opn_c);
  }

  //  the ack page itself
  //
  flo_u = eng_flow_make(eng, per_u, 0, DIRE_BAK);
  flo_u->las_d = 1;
  lin_u = eng_line_get(eng, pat_c);
  expect(0 != lin_u, "ack page");
  if ( lin_u ) {
    wan_z = _hex("10af4280009e9c36400b111e34dab", wan_y, 64);
    expect((lin_u->tob_d == wan_z) && !memcmp(lin_u->ser_y, wan_y, wan_z),
           "ack page ciphertext");
    _hex("a14ad4104c9fc64d07eb576f5f7abaa6", wan_y, 16);
    expect((AUTH_HMAC == lin_u->aut_u.typ_e) && !memcmp(lin_u->aut_u.mac_y, wan_y, 16),
           "ack page mac");
  }

  //  LSS roots
  //
  {
    lss_hash pof_u[1];
    c3_y     buf_y[3000];

    mesa_blake3(5, (c3_y*)"hello", 32, pof_u[0]);
    _hex("f20674a62c508e9868ab74ec1f86e50b34b8de5c591445e928286b33d168fea", wan_y, 32);
    expect(!memcmp(pof_u[0], wan_y, 32), "lss root (1 leaf)");

    memset(buf_y, 'x', sizeof(buf_y));
    {
      lss_builder* bil_u = c3_malloc(sizeof(*bil_u));
      lss_hash*    pof_y;
      c3_y         roo_y[32];

      lss_builder_init(bil_u, 3);
      lss_builder_ingest(bil_u, buf_y, 1024);
      lss_builder_ingest(bil_u, buf_y + 1024, 1024);
      lss_builder_ingest(bil_u, buf_y + 2048, 952);
      pof_y = lss_builder_finalize(bil_u);
      lss_recover_root(roo_y, pof_y, lss_proof_size(3));
      _hex("127c62ef8c0ef4ad0ae80a32bb1cd4dc6975018d72d0b5766b2f8d2f036c28b9",
           wan_y, 32);
      expect(!memcmp(roo_y, wan_y, 32), "lss root (3 leaves)");
      lss_builder_free(bil_u);
    }
  }

  c3_free(inn_c);
  c3_free(pat_c);
  mesa_engine_close(eng);
  mesa_store_free(cfg.store);
}

/* a simulated network
*/
#define NODES 5

typedef struct _dgram {
  mesa_lane      fro_u;
  mesa_lane      tar_u;
  c3_z           len_z;
  c3_y*          buf_y;
  struct _dgram* nex_u;
} _dgram;

typedef struct _rcv {
  c3_c*        kin_c;    //  "plea" "boon" "done" "sage" "corked"
  mesa_ship    her;
  c3_d         bon_d;
  mesa_noun    som;
  struct _rcv* nex_u;
} _rcv;

typedef struct _node {
  mesa_ship    who;
  mesa_ident   ide;
  mesa_store*  sto;
  mesa_engine* eng;
  mesa_lane    lan_u;
  _rcv*        rcv_u;
  c3_w         nak_w;    //  nack the next n pleas
  c3_w         dro_w;    //  drop the next n pleas
  c3_w         flu_w;    //  flub the next n pleas
  c3_d         kid_d;    //  the %shut key we give out (0: none)
  mesa_ship    kif_u;    //    to whom
  c3_o         bon_o;    //  answer each plea with a boon
  c3_o         lat_o;    //  answer pleas later
  c3_w         drp_w;    //  drop rate, percent
  mesa_ship    ask_u[16];
  c3_w         asn_w;
  c3_o         off_o;    //  down
  mesa_ship    mon_u;    //  a moon of ours whose keys we give out
  mesa_ident   mon_i;
  c3_d         jab_d;    //  the flow we give them on
  mesa_ship    jah_u;    //    to whom
} _node;

static _node   nod_u[NODES];
static c3_w    non_w;
static _dgram* net_u;
static _dgram** nen_u = &net_u;
static c3_d    now_d = 1000;
static c3_w    los_w;     //  global loss, percent
static c3_d    sen_d;     //  datagrams sent

static mesa_lane
_ip_lane(c3_w i)
{
  return (mesa_lane){ .kind = MESA_LANE_IP, .ip = 0x7f000001 + i, .port = 40000 + i };
}

static _node*
_node_at(mesa_lane lan_u)
{
  for ( c3_w i = 0; i < non_w; i++ ) {
    if ( MESA_LANE_GALAXY == lan_u.kind ) {
      if ( (MESA_CZAR == mesa_ship_rank(nod_u[i].who)) && (nod_u[i].who.lo == lan_u.galaxy) ) {
        return &nod_u[i];
      }
    }
    else if ( (lan_u.ip == nod_u[i].lan_u.ip) && (lan_u.port == nod_u[i].lan_u.port) ) {
      return &nod_u[i];
    }
  }
  return 0;
}

static void
_h_send(void* ctx, mesa_lane lane, const uint8_t* buf, size_t len)
{
  _node*  nod = ctx;
  _dgram* dgm_u = c3_calloc(sizeof(*dgm_u));

  dgm_u->fro_u = nod->lan_u;
  dgm_u->tar_u = lane;
  dgm_u->len_z = len;
  dgm_u->buf_y = c3_malloc(len);
  memcpy(dgm_u->buf_y, buf, len);
  *nen_u = dgm_u;
  nen_u  = &dgm_u->nex_u;
  sen_d++;
}

static void
_h_need(void* ctx, mesa_ship who)
{
  _node* nod = ctx;
  if ( nod->asn_w < 16 ) {
    nod->ask_u[nod->asn_w++] = who;
  }
}

static void
_record(_node* nod, const c3_c* kin_c, mesa_ship her, c3_d bon_d, mesa_noun som)
{
  _rcv*  rcv_u = c3_calloc(sizeof(*rcv_u));
  _rcv** end_u = &nod->rcv_u;

  rcv_u->kin_c = (c3_c*)kin_c;
  rcv_u->her   = her;
  rcv_u->bon_d = bon_d;
  rcv_u->som   = ( MESA_NONE == som ) ? MESA_NONE : mesa_ref(som);
  while ( *end_u ) end_u = &(*end_u)->nex_u;
  *end_u = rcv_u;
}

static mesa_noun
_ship_atom(mesa_ship who)
{
  c3_y buf_y[16];
  for ( c3_w i = 0; i < 8; i++ ) {
    buf_y[i]     = (c3_y)(who.lo >> (8 * i));
    buf_y[i + 8] = (c3_y)(who.hi >> (8 * i));
  }
  return mesa_atom_bytes(16, buf_y);
}

/* _moon_full(): jael's [%public-keys-result %full (map ship point)]
*/
static mesa_noun
_moon_full(_node* nod)
{
  c3_y      pas_y[65];
  mesa_noun pon;

  mesa_ident_pass(&nod->mon_i, pas_y);
  //  [rift life keys=(map life [suite pass]) sponsor=~]
  pon = mesa_qual(mesa_atom_u64(nod->mon_i.rift),
                  mesa_atom_u64(nod->mon_i.life),
                  mesa_trel(mesa_trel(mesa_atom_u64(nod->mon_i.life),
                                      mesa_atom_u64(1),
                                      mesa_atom_bytes(65, pas_y)),
                            mesa_atom_u64(0), mesa_atom_u64(0)),
                  mesa_atom_u64(0));
  return mesa_trel(mesa_atom_cord("public-keys-result"),
                   mesa_atom_cord("full"),
                   mesa_trel(mesa_cell(_ship_atom(nod->mon_u), pon),
                             mesa_atom_u64(0), mesa_atom_u64(0)));
}

static mesa_plea_result
_h_plea(void* ctx, mesa_ship her, uint64_t bone, mesa_noun vane,
        mesa_noun path, mesa_noun payload, mesa_noun* error)
{
  _node* nod = ctx;
  c3_d   van_d = 0;

  //  jael: [%0 %public-keys whos], answered with our moon's point
  //
  if ( mesa_atom_get_u64(vane, &van_d) && ('j' == van_d) ) {
    _record(nod, "jael", her, bone, payload);
    nod->jab_d = bone;
    nod->jah_u = her;
    mesa_flow_boon(nod->eng, her, bone, _moon_full(nod));
    return MESA_ACK;
  }

  if ( nod->dro_w ) {
    //  a boon from a dropped plea must be undone too
    nod->dro_w--;
    _record(nod, "drop", her, bone, payload);
    mesa_flow_boon(nod->eng, her, bone, mesa_atom_cord("undone"));
    return MESA_DROP;
  }
  if ( nod->flu_w ) {
    nod->flu_w--;
    _record(nod, "flub", her, bone, payload);
    mesa_flow_boon(nod->eng, her, bone, mesa_atom_cord("undone"));
    return MESA_FLUB;
  }
  if ( nod->nak_w ) {
    nod->nak_w--;
    *error = mesa_cell(mesa_atom_cord("nope"), mesa_atom_u64(0));
    _record(nod, "nack", her, bone, payload);
    return MESA_NACK;
  }
  _record(nod, "plea", her, bone, payload);

  if ( c3y == nod->lat_o ) {
    return MESA_LATER;
  }
  if ( c3y == nod->bon_o ) {
    mesa_flow_boon(nod->eng, her, bone,
                   mesa_cell(mesa_atom_cord("re"), mesa_ref(payload)));
  }
  return MESA_ACK;
}

static void
_h_boon(void* ctx, mesa_ship her, uint64_t bone, mesa_noun payload)
{
  _record(ctx, "boon", her, bone, payload);
}

static void
_h_done(void* ctx, mesa_ship her, uint64_t bone, mesa_noun error)
{
  _record(ctx, "done", her, bone, error);
}

static void
_h_corked(void* ctx, mesa_ship her, uint64_t bone)
{
  _record(ctx, "corked", her, bone, MESA_NONE);
}

static void
_h_breach(void* ctx, mesa_ship her)
{
  _record(ctx, "breach", her, 0, MESA_NONE);
}

static void
_h_sage(void* ctx, mesa_ship her, const char* path, mesa_noun gage)
{
  _record(ctx, "sage", her, 0, gage);
}

static uint64_t
_h_shut_key(void* ctx, mesa_ship her, const char* agent, const char* spur)
{
  _node* nod = ctx;
  _record(nod, "shut-key", her, 0, MESA_NONE);
  if (  nod->kid_d && mesa_ship_eq(her, nod->kif_u)
     && !strcmp(agent, "test") && !strncmp(spur, "/secret", 7) )
  {
    return nod->kid_d;
  }
  return 0;
}

static mesa_noun
_h_peek(void* ctx, const mesa_ship* her, const char* path)
{
  if ( !strncmp(path, "/g/x/", 5) && strstr(path, "/test//1/secret/hi") ) {
    return mesa_cell(mesa_atom_cord("noun"), mesa_atom_cord("psst"));
  }
  if ( !strncmp(path, "/g/x/", 5) && strstr(path, "/test//1/secret/big") ) {
    c3_y* buf_y = c3_malloc(20000);
    for ( c3_w i = 0; i < 20000; i++ ) buf_y[i] = (c3_y)(i * 17 + 3);
    mesa_noun big = mesa_atom_bytes(20000, buf_y);
    c3_free(buf_y);
    return mesa_cell(mesa_atom_cord("noun"), big);
  }
  if ( 0 == strcmp(path, "/g/x/0/test//hello") ) {
    return mesa_cell(mesa_atom_cord("noun"), mesa_atom_cord("world"));
  }
  if ( 0 == strcmp(path, "/g/x/0/test//big") ) {
    c3_y* buf_y = c3_malloc(20000);
    for ( c3_w i = 0; i < 20000; i++ ) buf_y[i] = (c3_y)(i * 31 + 7);
    mesa_noun big = mesa_atom_bytes(20000, buf_y);
    c3_free(buf_y);
    return mesa_cell(mesa_atom_cord("noun"), big);
  }
  return MESA_NONE;
}

static mesa_err
_node_open(_node* nod, c3_o fre_o)
{
  mesa_config cfg = {0};

  cfg.ident    = nod->ide;
  cfg.store    = nod->sto;
  cfg.breached = ( c3y == fre_o );
  cfg.no_ping  = true;
  cfg.now      = now_d;
  cfg.hooks    = (mesa_hooks){
    .ctx = nod, .send = _h_send, .need_point = _h_need, .plea = _h_plea,
    .boon = _h_boon, .done = _h_done, .corked = _h_corked, .sage = _h_sage,
    .peek = _h_peek, .breach = _h_breach, .shut_key = _h_shut_key,
  };
  return mesa_engine_open(&cfg, &nod->eng);
}

static _node*
_add_node(mesa_ident* ide)
{
  _node* nod = &nod_u[non_w];

  memset(nod, 0, sizeof(*nod));
  nod->who   = ide->who;
  nod->ide   = *ide;
  nod->sto   = mesa_store_memory();
  nod->lan_u = _ip_lane(non_w);
  nod->bon_o = c3n;
  nod->lat_o = c3n;
  nod->off_o = c3n;
  non_w++;
  u3_assert( MESA_OK == _node_open(nod, c3y) );
  return nod;
}

static _node*
_add_fake(const c3_c* who_c)
{
  mesa_ident ide;
  mesa_ident_fake(_ship(who_c), &ide);
  return _add_node(&ide);
}

static void
_reset_net(void)
{
  while ( net_u ) {
    _dgram* nex_u = net_u->nex_u;
    c3_free(net_u->buf_y);
    c3_free(net_u);
    net_u = nex_u;
  }
  nen_u = &net_u;
}

/* _answer_points(): resolve keys from the fake-ship table
*/
static void
_answer_points(void)
{
  for ( c3_w i = 0; i < non_w; i++ ) {
    _node* nod = &nod_u[i];
    while ( nod->asn_w && (c3n == nod->off_o) ) {
      mesa_ship  who = nod->ask_u[--nod->asn_w];
      c3_o       fon_o = c3n;

      for ( c3_w j = 0; j < non_w; j++ ) {
        if ( mesa_ship_eq(nod_u[j].who, who) && (MESA_PAWN != mesa_ship_rank(who)) ) {
          mesa_point pot = _fake_point(who);
          pot.rift = nod_u[j].ide.rift;
          pot.life = nod_u[j].ide.life;
          mesa_ident_pass(&nod_u[j].ide, pot.pass);
          mesa_engine_point(nod->eng, &pot);
          fon_o = c3y;
        }
      }
      if ( c3n == fon_o ) {
        mesa_point pot = _fake_point(who);   //  unknown ships: fake keys
        mesa_engine_point(nod->eng, &pot);
      }
    }
  }
}

/* _run(): deliver datagrams and fire timers until quiet or [max_d] ms
*/
static void
_run(c3_d max_d)
{
  c3_d end_d = now_d + max_d;

  while ( now_d < end_d ) {
    _answer_points();

    if ( net_u ) {
      _dgram* dgm_u = net_u;
      _node*  nod;

      net_u = dgm_u->nex_u;
      if ( !net_u ) nen_u = &net_u;

      nod = _node_at(dgm_u->tar_u);
      if (  nod && (c3n == nod->off_o)
         && ((rand() % 100) >= (c3_i)los_w)
         && ((rand() % 100) >= (c3_i)nod->drp_w) )
      {
        mesa_engine_hear(nod->eng, dgm_u->fro_u, dgm_u->buf_y, dgm_u->len_z, now_d);
      }
      c3_free(dgm_u->buf_y);
      c3_free(dgm_u);
      continue;
    }

    //  quiet: advance to the next timer
    //
    {
      c3_d dea_d = UINT64_MAX;
      for ( c3_w i = 0; i < non_w; i++ ) {
        if ( c3n == nod_u[i].off_o ) {
          dea_d = c3_min(dea_d, mesa_engine_deadline(nod_u[i].eng));
        }
      }
      if ( (UINT64_MAX == dea_d) || (dea_d > end_d) ) {
        now_d = end_d;
        break;
      }
      now_d = c3_max(now_d + 1, dea_d);
      for ( c3_w i = 0; i < non_w; i++ ) {
        if ( c3n == nod_u[i].off_o ) {
          mesa_engine_wake(nod_u[i].eng, now_d);
        }
      }
    }
  }
}

static c3_w
_count(_node* nod, const c3_c* kin_c)
{
  c3_w n_w = 0;
  for ( _rcv* rcv_u = nod->rcv_u; rcv_u; rcv_u = rcv_u->nex_u ) {
    if ( !strcmp(rcv_u->kin_c, kin_c) ) n_w++;
  }
  return n_w;
}

static _rcv*
_nth(_node* nod, const c3_c* kin_c, c3_w n_w)
{
  for ( _rcv* rcv_u = nod->rcv_u; rcv_u; rcv_u = rcv_u->nex_u ) {
    if ( !strcmp(rcv_u->kin_c, kin_c) && !n_w-- ) return rcv_u;
  }
  return 0;
}

static void
_clear(_node* nod)
{
  while ( nod->rcv_u ) {
    _rcv* nex_u = nod->rcv_u->nex_u;
    if ( MESA_NONE != nod->rcv_u->som ) mesa_unref(nod->rcv_u->som);
    c3_free(nod->rcv_u);
    nod->rcv_u = nex_u;
  }
}

static mesa_noun
_gpath(const c3_c* agent)
{
  return mesa_cell(mesa_atom_cord("ge"),
                   mesa_cell(mesa_atom_cord(agent), mesa_atom_u64(0)));
}

static mesa_err
_send(_node* fro, _node* tar, c3_d bon_d, mesa_noun pay)
{
  return mesa_flow_plea(fro->eng, tar->who, bon_d, mesa_atom_cord("g"),
                        _gpath("test"), pay);
}

/* tests
*/
static void
_test_plea(_node* zod, _node* nec)
{
  c3_d bon_d;

  expect(MESA_OK == mesa_flow_open(zod->eng, nec->who, &bon_d), "open");
  expect(MESA_OK == _send(zod, nec, bon_d, mesa_atom_cord("hello")), "plea");
  _run(60000);

  expect(1 == _count(nec, "plea"), "nec got %u pleas", _count(nec, "plea"));
  if ( _count(nec, "plea") ) {
    _rcv* rcv_u = _nth(nec, "plea", 0);
    mesa_noun hel = mesa_atom_cord("hello");
    expect(mesa_eq(rcv_u->som, hel), "plea payload");
    expect(mesa_ship_eq(rcv_u->her, zod->who), "plea sender");
    expect(rcv_u->bon_d == bon_d, "plea bone");
    mesa_unref(hel);
  }
  expect(1 == _count(zod, "done"), "zod got %u dones", _count(zod, "done"));
  if ( _count(zod, "done") ) {
    expect(MESA_NONE == _nth(zod, "done", 0)->som, "acked");
  }

  //  ten more, in order, exactly once
  //
  _clear(zod); _clear(nec);
  for ( c3_w i = 0; i < 10; i++ ) {
    _send(zod, nec, bon_d, mesa_atom_u64(i));
  }
  _run(60000);
  expect(10 == _count(nec, "plea"), "ten pleas: %u", _count(nec, "plea"));
  for ( c3_w i = 0; i < _count(nec, "plea"); i++ ) {
    c3_d val_d = 99;
    mesa_atom_get_u64(_nth(nec, "plea", i)->som, &val_d);
    expect(val_d == i, "order %u got %" PRIu64, i, val_d);
  }
  expect(10 == _count(zod, "done"), "ten dones: %u", _count(zod, "done"));
  _clear(zod); _clear(nec);
}

static void
_test_nack(_node* zod, _node* nec)
{
  c3_d bon_d;

  mesa_flow_open(zod->eng, nec->who, &bon_d);
  nec->nak_w = 1;
  _send(zod, nec, bon_d, mesa_atom_cord("bad"));
  _send(zod, nec, bon_d, mesa_atom_cord("good"));
  _run(60000);

  expect(1 == _count(nec, "nack") && 1 == _count(nec, "plea"), "nack then plea");
  expect(2 == _count(zod, "done"), "two dones: %u", _count(zod, "done"));
  if ( 2 == _count(zod, "done") ) {
    mesa_noun err = _nth(zod, "done", 0)->som;
    mesa_noun wan = mesa_cell(mesa_atom_cord("nope"), mesa_atom_u64(0));
    expect((MESA_NONE != err) && mesa_eq(err, wan), "naxplanation");
    expect(MESA_NONE == _nth(zod, "done", 1)->som, "then acked");
    mesa_unref(wan);
  }
  _clear(zod); _clear(nec);
}

static void
_test_boon(_node* zod, _node* nec)
{
  c3_d bon_d;

  mesa_flow_open(zod->eng, nec->who, &bon_d);
  nec->bon_o = c3y;
  _send(zod, nec, bon_d, mesa_atom_cord("sub"));
  _send(zod, nec, bon_d, mesa_atom_cord("sub2"));
  _run(60000);
  nec->bon_o = c3n;

  expect(2 == _count(zod, "boon"), "boons: %u", _count(zod, "boon"));
  if ( _count(zod, "boon") ) {
    mesa_noun wan = mesa_cell(mesa_atom_cord("re"), mesa_atom_cord("sub"));
    expect(mesa_eq(_nth(zod, "boon", 0)->som, wan), "boon payload");
    expect(_nth(zod, "boon", 0)->bon_d == bon_d, "boon bone");
    mesa_unref(wan);
  }
  _clear(zod); _clear(nec);
}

static mesa_noun
_big(c3_z len_z, c3_y sed_y)
{
  c3_y* buf_y = c3_malloc(len_z);
  for ( c3_z i = 0; i < len_z; i++ ) buf_y[i] = (c3_y)(i * 13 + sed_y);
  buf_y[len_z - 1] |= 1;
  mesa_noun big = mesa_atom_bytes(len_z, buf_y);
  c3_free(buf_y);
  return big;
}

static void
_test_big(_node* zod, _node* nec)
{
  c3_d      bon_d;
  mesa_noun big = _big(50000, 3);

  mesa_flow_open(zod->eng, nec->who, &bon_d);
  nec->bon_o = c3y;
  _send(zod, nec, bon_d, mesa_ref(big));
  _run(120000);
  nec->bon_o = c3n;

  expect(1 == _count(nec, "plea"), "big plea");
  if ( _count(nec, "plea") ) {
    expect(mesa_eq(_nth(nec, "plea", 0)->som, big), "big plea intact");
  }
  expect(1 == _count(zod, "boon"), "big boon");
  if ( _count(zod, "boon") ) {
    mesa_noun wan = mesa_cell(mesa_atom_cord("re"), mesa_ref(big));
    expect(mesa_eq(_nth(zod, "boon", 0)->som, wan), "big boon intact");
    mesa_unref(wan);
  }
  expect(1 == _count(zod, "done"), "big done");

  //  just over one fragment, so the poke goes over the MTU
  //
  _clear(zod); _clear(nec);
  mesa_unref(big);
  big = _big(1020, 5);
  _send(zod, nec, bon_d, mesa_ref(big));
  _run(60000);
  expect(1 == _count(nec, "plea") && mesa_eq(_nth(nec, "plea", 0)->som, big),
         "one-fragment over-mtu plea");
  mesa_unref(big);
  _clear(zod); _clear(nec);
}

static void
_test_keen(_node* zod, _node* nec)
{
  mesa_keen(zod->eng, nec->who, "/g/x/0/test//hello");
  mesa_keen(zod->eng, nec->who, "/g/x/0/test//big");
  _run(60000);

  expect(2 == _count(zod, "sage"), "sages: %u", _count(zod, "sage"));
  for ( c3_w i = 0; i < _count(zod, "sage"); i++ ) {
    mesa_noun gag = _nth(zod, "sage", i)->som;
    expect(MESA_NONE != gag && mesa_is_cell(gag), "sage gage");
    if ( MESA_NONE != gag && mesa_is_atom(mesa_tail(gag)) && 5 == mesa_met3(mesa_tail(gag)) ) {
      mesa_noun wan = mesa_cell(mesa_atom_cord("noun"), mesa_atom_cord("world"));
      expect(mesa_eq(gag, wan), "small sage");
      mesa_unref(wan);
    }
    else if ( MESA_NONE != gag ) {
      expect(20000 == mesa_met3(mesa_tail(gag)), "big sage size");
    }
  }
  _clear(zod); _clear(nec);
}

static void _restart(_node* nod);

/* %shut: a group key, given over /gk, reads what it covers; nothing
** else does
*/
static void
_test_shut(_node* zod, _node* nec)
{
  c3_y      key_y[32];
  c3_y      kez_y[32];
  c3_d      kid_d = 0, kiz_d = 0, bon_d;
  mesa_noun psst = mesa_cell(mesa_atom_cord("noun"), mesa_atom_cord("psst"));

  for ( c3_w i = 0; i < 32; i++ ) key_y[i] = (c3_y)(i * 7 + 1);
  expect(MESA_E_ARG == mesa_shut_add(nec->eng, "/a/x/1//flow", key_y, &kid_d),
         "no keys for ames");
  expect(MESA_OK == mesa_shut_add(nec->eng, "/g/x/0/test//1/secret", key_y,
                                  &kid_d), "shut add");
  expect(1 == kid_d, "first kid is 1: %" PRIu64, kid_d);

  //  covered paths are never public
  //
  mesa_keen(zod->eng, nec->who, "/g/x/0/test//1/secret/hi");
  _run(30000);
  expect(0 == _count(zod, "sage"), "not in the clear");
  mesa_yawn(zod->eng, nec->who, "/g/x/0/test//1/secret/hi");

  //  ask for the key; refused, then given
  //
  mesa_flow_open(zod->eng, nec->who, &bon_d);
  mesa_gall_key(zod->eng, nec->who, bon_d, "/g/x/3/test//1/secret/hi");
  _run(30000);
  expect(1 == _count(nec, "shut-key"), "asked the app: %u",
         _count(nec, "shut-key"));
  expect(1 == _count(zod, "boon"), "an answer: %u", _count(zod, "boon"));
  if ( _count(zod, "boon") ) {
    expect(!mesa_gall_key_parse(_nth(zod, "boon", 0)->som, &kiz_d, kez_y),
           "refused");
  }
  _clear(zod); _clear(nec);

  nec->kid_d = kid_d;
  nec->kif_u = zod->who;
  mesa_gall_key(zod->eng, nec->who, bon_d, "/g/x/3/test//1/secret/hi");
  _run(30000);
  expect(1 == _count(zod, "boon"), "an answer: %u", _count(zod, "boon"));
  expect(1 == _count(zod, "done"), "acked: %u", _count(zod, "done"));
  if ( _count(zod, "boon") ) {
    expect(mesa_gall_key_parse(_nth(zod, "boon", 0)->som, &kiz_d, kez_y),
           "a key");
    expect(kiz_d == kid_d && !memcmp(kez_y, key_y, 32), "the key");
  }
  _clear(zod); _clear(nec);

  //  read with it, small and large, at any case
  //
  mesa_keen_shut(zod->eng, nec->who, kiz_d, kez_y, "/g/x/3/test//1/secret/hi");
  mesa_keen_shut(zod->eng, nec->who, kiz_d, kez_y, "/g/x/4/test//1/secret/big");
  _run(60000);
  expect(2 == _count(zod, "sage"), "shut sages: %u", _count(zod, "sage"));
  for ( c3_w i = 0; i < _count(zod, "sage"); i++ ) {
    _rcv* rcv_u = _nth(zod, "sage", i);
    if ( MESA_NONE == rcv_u->som ) {
      expect(0, "shut sage %u empty", i);
    }
    else if ( 20000 == mesa_met3(mesa_tail(rcv_u->som)) ) {
      expect(1, "big");
    }
    else {
      expect(mesa_eq(rcv_u->som, psst), "small shut sage");
    }
  }
  _clear(zod); _clear(nec);

  //  a wrong key, or a path the key doesn't cover: no answer
  //
  kez_y[0] ^= 1;
  mesa_keen_shut(zod->eng, nec->who, kiz_d, kez_y, "/g/x/5/test//1/secret/hi");
  mesa_keen_shut(zod->eng, nec->who, kiz_d, key_y, "/g/x/5/test//hello");
  _run(30000);
  expect(0 == _count(zod, "sage"), "wrong key or path: %u",
         _count(zod, "sage"));
  mesa_yawn(zod->eng, nec->who, "/g/x/5/test//1/secret/hi");
  mesa_yawn(zod->eng, nec->who, "/g/x/5/test//hello");

  //  keys survive a restart; the next kid follows
  //
  _restart(nec);
  mesa_keen_shut(zod->eng, nec->who, kid_d, key_y, "/g/x/6/test//1/secret/hi");
  _run(30000);
  expect(1 == _count(zod, "sage"), "after restart: %u", _count(zod, "sage"));
  expect(MESA_OK == mesa_shut_add(nec->eng, "/g/x/0/test//1/other", key_y,
                                  &kiz_d) && (2 == kiz_d), "next kid");

  nec->kid_d = 0;
  mesa_unref(psst);
  _clear(zod); _clear(nec);
}

static void
_test_cork(_node* zod, _node* nec)
{
  c3_d bon_d;

  mesa_flow_open(zod->eng, nec->who, &bon_d);
  _send(zod, nec, bon_d, mesa_atom_cord("before"));
  _run(30000);
  expect(MESA_OK == mesa_flow_cork(zod->eng, nec->who, bon_d), "cork");
  expect(MESA_E_STATE == _send(zod, nec, bon_d, mesa_atom_cord("after")),
         "no pleas once closing");
  _run(60000);

  expect(1 == _count(zod, "corked"), "corked hook");
  {
    eng_peer* zop = eng_peer_get(zod->eng, (u3_ship){ nec->who.lo, nec->who.hi });
    eng_peer* nep = eng_peer_get(nec->eng, (u3_ship){ zod->who.lo, zod->who.hi });
    expect(!eng_flow_get(zop, bon_d, DIRE_FOR), "zod flow gone");
    expect(c3y == eng_flow_corked(zop, bon_d, DIRE_FOR), "zod remembers cork");
    expect(!eng_flow_get(nep, bon_d, DIRE_BAK), "nec flow gone");
    expect(c3y == eng_flow_corked(nep, bon_d, DIRE_BAK), "nec remembers cork");
  }
  _clear(zod); _clear(nec);
}

static void
_test_loss(_node* zod, _node* nec)
{
  c3_d      bon_d;
  mesa_noun big = _big(30000, 9);

  los_w = 30;
  mesa_flow_open(zod->eng, nec->who, &bon_d);
  nec->bon_o = c3y;
  for ( c3_w i = 0; i < 20; i++ ) {
    _send(zod, nec, bon_d, mesa_atom_u64(100 + i));
  }
  _send(zod, nec, bon_d, mesa_ref(big));
  _run(30 * 60 * 1000);
  los_w = 0;
  nec->bon_o = c3n;

  expect(21 == _count(nec, "plea"), "lossy pleas: %u", _count(nec, "plea"));
  for ( c3_w i = 0; i < 20 && i < _count(nec, "plea"); i++ ) {
    c3_d val_d = 0;
    mesa_atom_get_u64(_nth(nec, "plea", i)->som, &val_d);
    expect(val_d == 100 + i, "lossy order %u", i);
  }
  if ( 21 == _count(nec, "plea") ) {
    expect(mesa_eq(_nth(nec, "plea", 20)->som, big), "lossy big");
  }
  expect(21 == _count(zod, "done"), "lossy dones: %u", _count(zod, "done"));
  expect(21 == _count(zod, "boon"), "lossy boons: %u", _count(zod, "boon"));
  mesa_unref(big);
  _clear(zod); _clear(nec);
}

/* restart: crash both sides mid-stream; nothing lost or repeated
*/
static void
_restart(_node* nod)
{
  mesa_engine_close(nod->eng);
  nod->eng = 0;
  u3_assert( MESA_OK == _node_open(nod, c3n) );
}

static void
_test_restart(_node* zod, _node* nec)
{
  c3_d bon_d;

  mesa_flow_open(zod->eng, nec->who, &bon_d);
  nec->bon_o = c3y;
  for ( c3_w i = 0; i < 30; i++ ) {
    _send(zod, nec, bon_d, mesa_atom_u64(200 + i));
  }

  for ( c3_w r = 0; r < 6; r++ ) {
    _run(1500);
    _restart(( r & 1 ) ? zod : nec);
    _reset_net();   //  in-flight datagrams die with the process
  }
  _run(30 * 60 * 1000);
  nec->bon_o = c3n;

  expect(30 == _count(nec, "plea"), "restart pleas: %u", _count(nec, "plea"));
  for ( c3_w i = 0; i < _count(nec, "plea"); i++ ) {
    c3_d val_d = 0;
    mesa_atom_get_u64(_nth(nec, "plea", i)->som, &val_d);
    expect(val_d == 200 + i, "restart order %u: %" PRIu64, i, val_d);
  }
  expect(30 == _count(zod, "done"), "restart dones: %u", _count(zod, "done"));
  expect(30 == _count(zod, "boon"), "restart boons: %u", _count(zod, "boon"));
  _clear(zod); _clear(nec);
}

/* MESA_DROP: no answer; the resend is handled
*/
static void
_test_drop(_node* zod, _node* nec)
{
  c3_d bon_d;

  mesa_flow_open(zod->eng, nec->who, &bon_d);
  nec->dro_w = 2;
  nec->bon_o = c3y;
  _send(zod, nec, bon_d, mesa_atom_cord("not yet"));
  _run(5 * 60 * 1000);
  nec->bon_o = c3n;

  expect(2 == _count(nec, "drop"), "dropped twice: %u", _count(nec, "drop"));
  expect(1 == _count(nec, "plea"), "then taken: %u", _count(nec, "plea"));
  expect(1 == _count(zod, "done"), "one answer: %u", _count(zod, "done"));
  if ( _count(zod, "done") ) {
    expect(MESA_NONE == _nth(zod, "done", 0)->som, "an ack");
  }
  expect(1 == _count(zod, "boon"), "dropped boons undone: %u", _count(zod, "boon"));
  _clear(zod); _clear(nec);
}

/* MESA_FLUB: the sender stops until we spur, across a restart
*/
static void
_test_flub_ours(_node* zod, _node* nec)
{
  c3_d bon_d, sen_d0;

  mesa_flow_open(zod->eng, nec->who, &bon_d);
  nec->flu_w = 1;
  _send(zod, nec, bon_d, mesa_atom_cord("anyone home?"));
  _run(5000);
  expect(1 == _count(nec, "flub"), "flubbed: %u", _count(nec, "flub"));
  expect(0 == _count(zod, "done"), "no answer");

  sen_d0 = sen_d;
  _run(30 * 60 * 1000);
  expect(sen_d - sen_d0 < 3, "sender halted: %" PRIu64 " datagrams",
         sen_d - sen_d0);
  expect(1 == _count(nec, "flub"), "not asked again: %u", _count(nec, "flub"));

  _restart(nec);
  expect(MESA_OK == mesa_spur(nec->eng, "other"), "spur other");
  _run(60000);
  expect(0 == _count(nec, "plea"), "other agent: still halted");

  expect(MESA_OK == mesa_spur(nec->eng, "test"), "spur");
  _run(5 * 60 * 1000);
  expect(1 == _count(nec, "plea"), "resent once spurred: %u",
         _count(nec, "plea"));
  expect(1 == _count(zod, "done"), "answered: %u", _count(zod, "done"));
  if ( _count(zod, "done") ) {
    expect(MESA_NONE == _nth(zod, "done", 0)->som, "an ack");
  }
  expect(0 == _count(zod, "boon"), "no boons for the app: %u",
         _count(zod, "boon"));
  expect(MESA_E_ARG == mesa_spur(nec->eng, ""), "spur needs an agent");
  _clear(zod); _clear(nec);
}

/* MESA_LATER: ack after the handler returns
*/
static void
_test_later(_node* zod, _node* nec)
{
  c3_d bon_d;

  mesa_flow_open(zod->eng, nec->who, &bon_d);
  nec->lat_o = c3y;
  _send(zod, nec, bon_d, mesa_atom_cord("slow"));
  _run(5000);
  expect(1 == _count(nec, "plea") && 0 == _count(zod, "done"), "pending");

  nec->lat_o = c3n;
  expect(MESA_OK == mesa_flow_done(nec->eng, zod->who, bon_d, MESA_NONE), "done later");
  _run(5 * 60 * 1000);
  expect(1 == _count(zod, "done"), "later ack: %u", _count(zod, "done"));
  _clear(zod); _clear(nec);
}

/* a comet talks to a galaxy that's never heard of it
*/
static void
_test_comet(_node* nec)
{
  mesa_ident ide;
  mesa_ship  stars[1] = { _ship("~marnec") };
  c3_y       eny_y[64];
  _node*     com;
  c3_d       bon_d;

  for ( c3_w i = 0; i < 64; i++ ) eny_y[i] = (c3_y)(i * 5 + 3);
  u3_assert( mesa_comet_mine(1, stars, eny_y, 1000000, &ide) );
  com = _add_node(&ide);

  mesa_flow_open(com->eng, nec->who, &bon_d);
  nec->bon_o = c3y;
  _send(com, nec, bon_d, mesa_atom_cord("from a comet"));
  _run(5 * 60 * 1000);
  nec->bon_o = c3n;

  expect(1 == _count(nec, "plea"), "comet plea: %u", _count(nec, "plea"));
  expect(1 == _count(com, "done"), "comet done: %u", _count(com, "done"));
  expect(1 == _count(com, "boon"), "comet boon: %u", _count(com, "boon"));
  _clear(com); _clear(nec);
}

/* a moon: its keys come from its parent, which keeps them current
*/
static void
_test_moon(_node* zod, _node* nec)
{
  mesa_ship  mon = { .lo = (0x7a3bULL << 32) | nec->who.lo, .hi = 0 };
  mesa_ident ide;
  _node*     moo;
  c3_d       bon_d, bak_d;
  c3_w       ask_w = zod->asn_w;

  mesa_ident_fake(mon, &ide);
  nec->mon_u = mon;
  nec->mon_i = ide;
  moo = _add_node(&ide);
  expect(MESA_EARL == mesa_ship_rank(mon), "moon rank");

  //  the moon pokes ~zod, who asks ~nec (not azimuth) for its keys
  //
  mesa_flow_open(moo->eng, zod->who, &bon_d);
  _send(moo, zod, bon_d, mesa_atom_cord("from a moon"));
  _run(5 * 60 * 1000);

  expect(1 == _count(nec, "jael"), "parent asked once: %u", _count(nec, "jael"));
  expect(zod->asn_w == ask_w, "azimuth not asked about the moon");
  expect(1 == _count(zod, "plea"), "moon plea: %u", _count(zod, "plea"));
  expect(1 == _count(moo, "done"), "moon done: %u", _count(moo, "done"));
  if ( _count(nec, "jael") ) {
    //  [%0 %public-keys [moon ~ ~]]
    mesa_noun pay = _nth(nec, "jael", 0)->som;
    mesa_noun wan = mesa_trel(mesa_atom_u64(0), mesa_atom_cord("public-keys"),
                              mesa_trel(_ship_atom(mon), mesa_atom_u64(0),
                                        mesa_atom_u64(0)));
    expect(mesa_eq(pay, wan), "jael plea shape");
    mesa_unref(wan);
  }

  //  and ~zod can poke the moon back
  //
  mesa_flow_open(zod->eng, mon, &bak_d);
  _send(zod, moo, bak_d, mesa_atom_cord("to a moon"));
  _run(5 * 60 * 1000);
  expect(1 == _count(moo, "plea"), "zod to moon: %u", _count(moo, "plea"));
  expect(1 == _count(zod, "done"), "zod done: %u", _count(zod, "done"));
  _clear(zod); _clear(moo);

  //  the moon cycles its keys; its parent tells ~zod
  //
  {
    mesa_ident nid;
    c3_y       old_y[65], new_y[65];

    mesa_ident_fake(_ship("~bud"), &nid);   //  any other keys
    nid.who  = mon;
    nid.life = 2;
    mesa_ident_pass(&ide, old_y);
    mesa_ident_pass(&nid, new_y);

    mesa_flow_boon(nec->eng, nec->jah_u, nec->jab_d,
      mesa_trel(mesa_atom_cord("public-keys-result"), mesa_atom_cord("diff"),
        mesa_cell(_ship_atom(mon),
          mesa_trel(mesa_atom_cord("keys"),
                    mesa_trel(mesa_atom_u64(1), mesa_atom_u64(1),
                              mesa_atom_bytes(65, old_y)),
                    mesa_trel(mesa_atom_u64(2), mesa_atom_u64(1),
                              mesa_atom_bytes(65, new_y))))));
    _run(60000);

    mesa_engine_close(moo->eng);
    moo->ide = nid;
    nec->mon_i = nid;
    u3_assert( MESA_OK == _node_open(moo, c3n) );

    _send(zod, moo, bak_d, mesa_atom_cord("new keys"));
    _run(5 * 60 * 1000);
    expect(1 == _count(moo, "plea"), "after rekey: %u", _count(moo, "plea"));
    expect(1 == _count(zod, "done"), "rekey done: %u", _count(zod, "done"));
    _clear(zod); _clear(moo);
  }

  //  the moon breaches
  //
  mesa_flow_boon(nec->eng, nec->jah_u, nec->jab_d,
    mesa_trel(mesa_atom_cord("public-keys-result"), mesa_atom_cord("diff"),
      mesa_cell(_ship_atom(mon),
        mesa_trel(mesa_atom_cord("rift"), mesa_atom_u64(0), mesa_atom_u64(1)))));
  _run(60000);
  expect(1 == _count(zod, "breach"), "breach: %u", _count(zod, "breach"));
  if ( _count(zod, "breach") ) {
    expect(mesa_ship_eq(_nth(zod, "breach", 0)->her, mon), "breach who");
  }

  //  and comes back from fresh state, at its new rift
  //
  mesa_engine_close(moo->eng);
  mesa_store_free(moo->sto);
  moo->sto = mesa_store_memory();
  moo->ide.rift = 1;
  u3_assert( MESA_OK == _node_open(moo, c3y) );
  mesa_flow_open(moo->eng, zod->who, &bon_d);
  _send(moo, zod, bon_d, mesa_atom_cord("reborn"));
  _run(5 * 60 * 1000);
  expect(1 == _count(zod, "plea"), "after breach: %u", _count(zod, "plea"));
  expect(1 == _count(moo, "done"), "breach done: %u", _count(moo, "done"));
  _clear(zod); _clear(nec); _clear(moo);
}

/* flubs: a peer whose agent isn't running halts our flow, then resumes it
*/
static void
_test_flub(_node* zod, _node* moo)
{
  c3_d bon_d, sen_d0;
  c3_d gfb_d = 4;   //  the moon's first gall flow to ~zod was bone 0

  mesa_flow_open(moo->eng, zod->who, &bon_d);
  zod->lat_o = c3y;   //  their agent sits on it
  _send(moo, zod, bon_d, mesa_atom_cord("asleep?"));
  _run(5000);
  expect(1 == _count(zod, "plea"), "flub: delivered once");

  //  %flub: stop resending
  //
  mesa_flow_boon(zod->eng, moo->who, gfb_d,
                 mesa_qual(mesa_atom_u64(0), mesa_atom_cord("flub"),
                           mesa_atom_cord("test"), mesa_atom_u64(bon_d)));
  _run(5000);
  sen_d0 = sen_d;
  _run(30 * 60 * 1000);
  expect(sen_d - sen_d0 < 3, "halted: %" PRIu64 " datagrams", sen_d - sen_d0);
  expect(0 == _count(moo, "boon"), "flub boons stay in the engine");

  //  %spur: go on
  //
  zod->lat_o = c3n;
  mesa_flow_done(zod->eng, moo->who, bon_d, MESA_NONE);
  mesa_flow_boon(zod->eng, moo->who, gfb_d,
                 mesa_trel(mesa_atom_u64(0), mesa_atom_cord("spur"),
                           mesa_atom_cord("test")));
  _run(5 * 60 * 1000);
  expect(1 == _count(moo, "done"), "resumed: %u dones", _count(moo, "done"));
  expect(1 == _count(zod, "plea"), "still once: %u", _count(zod, "plea"));
  _clear(zod); _clear(moo);
}

/* rows in [nod]'s store under [typ_y] and [who]
*/
static int
_count_row(void* arg, const uint8_t* key_y, size_t key_z,
           const uint8_t* val_y, size_t val_z)
{
  (*(c3_w*)arg)++;
  return 0;
}

static c3_w
_rows(_node* nod, c3_y typ_y, mesa_ship who)
{
  c3_y pre_y[40];
  c3_w n_w = 0;
  eng_key(typ_y, (u3_ship){ who.lo, who.hi }, 0, 0, 0, pre_y);
  nod->sto->scan(nod->sto->ctx, pre_y, 17, _count_row, &n_w);
  return n_w;
}

static c3_o
_loaded(_node* nod, mesa_ship who)
{
  return __( !eng_peer_map_is_end(eng_peer_map_get(&nod->eng->per_u,
                                    (u3_ship_key){ .d = { who.lo, who.hi } })) );
}

/* garbage: corks as ranges, idle peers unloaded, moot peers forgotten
*/
static void
_test_gc(_node* zod, _node* nec, _node* moo)
{
  c3_d      bon_d[10], las_d, pre_d;
  eng_peer* nep;
  c3_w      cor_w;
  mesa_ship sam = _ship("~sampel-palnet");

  //  ten flows, each corked: one range for nec, no rows for zod
  //
  nep   = eng_peer_get(nec->eng, (u3_ship){ zod->who.lo, zod->who.hi });
  cor_w = eng_cork_count(nep);
  for ( c3_w i = 0; i < 10; i++ ) {
    mesa_flow_open(zod->eng, nec->who, &bon_d[i]);
    _send(zod, nec, bon_d[i], mesa_atom_u64(i));
    mesa_flow_cork(zod->eng, nec->who, bon_d[i]);
  }
  _run(5 * 60 * 1000);
  expect(10 == _count(zod, "corked"), "corked: %u", _count(zod, "corked"));
  nep = eng_peer_get(nec->eng, (u3_ship){ zod->who.lo, zod->who.hi });
  expect(eng_cork_count(nep) <= cor_w + 1, "ranges: %u (were %u)",
         eng_cork_count(nep), cor_w);
  expect(_rows(nec, KEY_CORK, zod->who) == eng_cork_count(nep),
         "a row per range: %u", _rows(nec, KEY_CORK, zod->who));
  for ( c3_w i = 0; i < 10; i++ ) {
    eng_peer* zop = eng_peer_get(zod->eng, (u3_ship){ nec->who.lo, nec->who.hi });
    expect(c3y == eng_flow_corked(zop, bon_d[i], DIRE_FOR), "ours %u", i);
    expect(c3y == eng_flow_corked(nep, bon_d[i], DIRE_BAK), "theirs %u", i);
  }
  _restart(nec);
  nep = eng_peer_get(nec->eng, (u3_ship){ zod->who.lo, zod->who.hi });
  expect(c3y == eng_flow_corked(nep, bon_d[4], DIRE_BAK), "after restart");
  expect(c3n == eng_flow_corked(nep, bon_d[9] + 4, DIRE_BAK), "not past it");
  _clear(zod); _clear(nec);

  //  a peer we only have keys for: forgotten once idle
  //
  {
    mesa_point pot = _fake_point(sam);
    mesa_point out;
    mesa_engine_point(zod->eng, &pot);
    expect(mesa_engine_peer(zod->eng, sam, &out), "keys for ~sampel-palnet");
  }

  //  quiet: the moon leaves zod's memory, ~sampel-palnet its store
  //
  pre_d = eng_peer_get(zod->eng, (u3_ship){ moo->who.lo, moo->who.hi })->bon_d;
  _run(15 * 60 * 1000);
  expect(c3n == _loaded(zod, moo->who), "moon unloaded");
  expect(c3n == _loaded(zod, sam), "~sampel-palnet unloaded");
  expect(0 == _rows(zod, KEY_PEER, sam), "~sampel-palnet forgotten");
  expect(1 == _rows(zod, KEY_PEER, moo->who), "the moon remembered");
  expect(c3y == _loaded(zod, nec->who), "galaxies stay");

  //  and comes back when we need it, on a fresh bone
  //
  {
    eng_peer* mop;
    mesa_point out;

    expect(!mesa_engine_peer(zod->eng, sam, &out), "nothing on ~sampel-palnet");
    expect(MESA_OK == mesa_flow_open(zod->eng, moo->who, &las_d), "open");
    mop = eng_peer_get(zod->eng, (u3_ship){ moo->who.lo, moo->who.hi });
    expect(mop && (c3y == mop->kno_o), "reloaded with keys");
    expect(las_d == pre_d, "next-bone kept: %" PRIu64, las_d);
    _send(zod, moo, las_d, mesa_atom_cord("still there?"));
    _run(5 * 60 * 1000);
    expect(1 == _count(moo, "plea"), "moon heard: %u", _count(moo, "plea"));
    expect(1 == _count(zod, "done"), "acked: %u", _count(zod, "done"));
  }
  _clear(zod); _clear(nec); _clear(moo);
}

/* gall's system flows: /gk gets [%0 ~] then an ack, /gf an ack; the
** application never sees either
*/
static void
_test_gall_sys(_node* zod, _node* nec)
{
  c3_d bon_d, bof_d;

  mesa_flow_open(zod->eng, nec->who, &bon_d);
  mesa_flow_plea(zod->eng, nec->who, bon_d, mesa_atom_cord("g"),
                 mesa_cell(mesa_atom_cord("gk"),
                           mesa_cell(mesa_atom_cord("test"), mesa_atom_u64(0))),
                 mesa_cell(mesa_atom_u64(0),
                           mesa_cell(mesa_atom_cord("foo"), mesa_atom_u64(0))));
  mesa_flow_open(zod->eng, nec->who, &bof_d);
  mesa_flow_plea(zod->eng, nec->who, bof_d, mesa_atom_cord("g"),
                 mesa_cell(mesa_atom_cord("gf"), mesa_atom_u64(0)),
                 mesa_cell(mesa_atom_u64(0), mesa_atom_u64(0)));
  _run(60000);

  expect(0 == _count(nec, "plea"), "system pleas stay in the engine");
  expect(1 == _count(zod, "boon"), "/gk boon: %u", _count(zod, "boon"));
  if ( _count(zod, "boon") ) {
    mesa_noun wan = mesa_cell(mesa_atom_u64(0), mesa_atom_u64(0));
    expect(mesa_eq(_nth(zod, "boon", 0)->som, wan), "/gk boon is [%%0 ~]");
    mesa_unref(wan);
  }
  expect(2 == _count(zod, "done"), "system acks: %u", _count(zod, "done"));
  for ( c3_w i = 0; i < _count(zod, "done"); i++ ) {
    expect(MESA_NONE == _nth(zod, "done", i)->som, "system ack %u", i);
  }
  _clear(zod); _clear(nec);
}

/* classic ames: the %ahoy we send (Arvo's +sift-shot and ++sivc decode it
** to [%$ /mesa-2 %ahoy ~]; checked once with `urbit eval`)
*/
static c3_y ahy_y[PACT_SIZE];
static c3_z ahz_z;

static void
_h_grab(void* ctx, mesa_lane lane, const uint8_t* buf, size_t len)
{
  memcpy(ahy_y, buf, len);
  ahz_z = len;
}

static void
_test_ames(void)
{
  static const c3_c* pac_c = "8e52901832b0cdcd8276c5cc2fae0da6cfead9f30014f0d7b07866cf574b975e2b66a9bfbcb600387ad2d58b263f00387ad2619b461300";
  mesa_ident   moo, mop;
  mesa_ship    m1, m2;
  mesa_config  cfg = {0};
  mesa_engine* eng;
  mesa_point   pot = {0};
  c3_y         wan_y[PACT_SIZE];
  c3_z         wan_z = strlen(pac_c) / 2;
  eng_peer*    per_u;

  for ( c3_z i = 0; i < wan_z; i++ ) {
    unsigned v;
    sscanf(pac_c + 2 * i, "%2x", &v);
    wan_y[wan_z - 1 - i] = (c3_y)v;
  }

  mesa_ship_parse("~foprev-satpur-mopfel-winrux", &m1);
  mesa_ship_parse("~mopfel-winrux", &m2);
  mesa_ident_fake(m1, &moo);
  mesa_ident_fake(m2, &mop);
  cfg.ident    = moo;
  cfg.store    = mesa_store_memory();
  cfg.breached = true;
  cfg.no_ping  = true;
  cfg.hooks.send = _h_grab;
  u3_assert( MESA_OK == mesa_engine_open(&cfg, &eng) );
  pot.who  = m2;
  pot.rift = 1;
  pot.life = 6;
  mesa_ident_pass(&mop, pot.pass);
  mesa_engine_point(eng, &pot);
  per_u = eng_peer_get(eng, (u3_ship){ m2.lo, m2.hi });

  eng_txn_open(eng);
  eng_ames_ahoy(eng, per_u, (mesa_lane){ .kind = MESA_LANE_IP, .ip = 1, .port = 1 });
  eng_txn_close(eng);
  expect((ahz_z == wan_z) && !memcmp(ahy_y, wan_y, wan_z), "%%ahoy packet");

  //  our own packet, heard back: not from them to us, so ignored
  //
  ahz_z = 0;
  mesa_engine_hear(eng, (mesa_lane){ .kind = MESA_LANE_IP, .ip = 2, .port = 2 },
                   wan_y, wan_z, 1000);
  expect(0 == ahz_z, "our own packet ignored");

  mesa_engine_close(eng);
  mesa_store_free(cfg.store);
}

/* garbage datagrams never crash or change anything
*/
static void
_test_garbage(_node* nec)
{
  c3_y buf_y[PACT_SIZE];

  srand(11);
  for ( c3_w i = 0; i < 200000; i++ ) {
    c3_z len_z = 8 + (rand() % 200);
    for ( c3_z j = 0; j < len_z; j++ ) buf_y[j] = (c3_y)rand();
    //  sometimes a valid header, to get past the cookie
    //
    if ( i & 1 ) {
      buf_y[4] = 0x00; buf_y[5] = 0x02; buf_y[6] = 0xe0; buf_y[7] = 0x67;
    }
    mesa_engine_hear(nec->eng, _ip_lane(9), buf_y, len_z, now_d);
  }
  _reset_net();
}

int
main(int argc, char* argv[])
{
  _node *zod, *nec;

  _test_vectors();
  _test_ames();

  srand(1);
  zod = _add_fake("~zod");
  nec = _add_fake("~nec");

  _test_plea(zod, nec);
  _test_nack(zod, nec);
  _test_boon(zod, nec);
  _test_big(zod, nec);
  _test_keen(zod, nec);
  _test_shut(zod, nec);
  _test_cork(zod, nec);
  _test_later(zod, nec);
  _test_drop(zod, nec);
  _test_flub_ours(zod, nec);
  _test_loss(zod, nec);
  _test_restart(zod, nec);
  _test_comet(nec);
  _test_gall_sys(zod, nec);
  _test_moon(zod, nec);
  _test_flub(zod, &nod_u[non_w - 1]);
  _test_gc(zod, nec, &nod_u[non_w - 1]);
  _test_garbage(nec);

  fprintf(stderr, "engine: %" PRIu64 " datagrams\r\n", sen_d);

  for ( c3_w i = 0; i < non_w; i++ ) {
    _clear(&nod_u[i]);
    mesa_engine_close(nod_u[i].eng);
    mesa_store_free(nod_u[i].sto);
  }
  _reset_net();

  if ( ret_i ) {
    fprintf(stderr, "engine: failed\r\n");
  }
  else {
    fprintf(stderr, "engine ok\r\n");
  }
  return ret_i;
}

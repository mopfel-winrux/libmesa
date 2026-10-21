/// @file
///
/// The public engine: boot, inputs, timers and the flow API.

#include "eng.h"
#include "mesa/ident.h"

static mesa_lane
_lane_none(void)
{
  return (mesa_lane){ .kind = MESA_LANE_NONE };
}

/* _peers(): snapshot the peer table (sending can add peers)
*/
static eng_peer**
_peers(mesa_engine* eng, c3_z* len_z)
{
  c3_z       n_z = eng_peer_map_size(&eng->per_u);
  eng_peer** per_u = c3_malloc(c3_max(n_z, 1) * sizeof(eng_peer*));
  c3_z       i_z = 0;

  for ( eng_peer_map_itr itr_u = eng_peer_map_first(&eng->per_u);
        !eng_peer_map_is_end(itr_u);
        itr_u = eng_peer_map_next(itr_u) )
  {
    per_u[i_z++] = itr_u.data->val;
  }
  *len_z = i_z;
  return per_u;
}

/* loading state from the store
*/
static c3_d
_get_be(const c3_y* buf_y)
{
  c3_d val_d = 0;
  for ( c3_w i = 0; i < 8; i++ ) {
    val_d = (val_d << 8) | buf_y[i];
  }
  return val_d;
}

static void
_key_ship(const c3_y* key_y, u3_ship who_u)
{
  who_u[1] = _get_be(key_y + 1);
  who_u[0] = _get_be(key_y + 9);
}

static c3_d
_u64(mesa_noun som)
{
  c3_d val_d = 0;
  if ( MESA_NONE != som ) {
    mesa_atom_get_u64(som, &val_d);
  }
  return val_d;
}

typedef struct {
  mesa_engine* eng;
  c3_o         bad_o;
} _load_ctx;

static int
_load_row(void* arg, const uint8_t* key_y, size_t key_z,
          const uint8_t* val_y, size_t val_z)
{
  _load_ctx*   ctx_u = arg;
  mesa_engine* eng   = ctx_u->eng;
  mesa_noun    val   = mesa_cue_bytes(val_z, val_y);
  u3_ship      who_u;
  eng_peer*    per_u;

  if ( MESA_NONE == val ) {
    ctx_u->bad_o = c3y;
    return 1;
  }

  switch ( key_y[0] ) {
    case KEY_PEER: {
      c3_y pas_y[65];

      if ( 17 != key_z ) break;
      _key_ship(key_y, who_u);
      per_u = eng_peer_make(eng, who_u);

      per_u->bon_d = _u64(mesa_at(15, val));
      if ( mesa_is_cell(mesa_at(14, val)) ) {
        c3_y spo_y[16];
        mesa_atom_get_bytes(mesa_at(29, val), 16, spo_y);
        u3_ship_of_bytes(per_u->spo_u, 16, spo_y);
        per_u->spo_o = c3y;
      }
      if ( _u64(mesa_at(2, val)) ) {
        mesa_atom_get_bytes(mesa_at(27, val), 65, pas_y);
        eng_peer_set_keys(eng, per_u, _u64(mesa_at(12, val)),
                          _u64(mesa_at(26, val)), pas_y);
        per_u->kno_o = c3y;
      }
    } break;

    case KEY_FLOW: {
      eng_flow* flo_u;
      mesa_noun nax;

      if ( 26 != key_z ) break;
      _key_ship(key_y, who_u);
      per_u = eng_peer_make(eng, who_u);
      flo_u = eng_flow_make(eng, per_u, _get_be(key_y + 17),
                            (eng_dire)key_y[25]);

      flo_u->clo_o = _u64(mesa_at(4, val)) ? c3y : c3n;
      flo_u->hat_o = _u64(mesa_at(10, val)) ? c3y : c3n;
      flo_u->nex_d = _u64(mesa_at(22, val));
      flo_u->win_w = (c3_w)_u64(mesa_at(23, val));
      flo_u->las_d = _u64(mesa_at(6, val));
      flo_u->pen_o = _u64(mesa_at(14, val)) ? c3y : c3n;

      for ( nax = mesa_at(15, val);
            (MESA_NONE != nax) && mesa_is_cell(nax) && (flo_u->nan_w < 10);
            nax = mesa_tail(nax) )
      {
        flo_u->nax_u[flo_u->nan_w].seq_d = _u64(mesa_head(mesa_head(nax)));
        flo_u->nax_u[flo_u->nan_w].err   = mesa_ref(mesa_tail(mesa_head(nax)));
        flo_u->nan_w++;
      }
    } break;

    case KEY_LOAD: {
      eng_flow* flo_u;
      eng_msg*  msg_u;
      eng_msg** end_u;

      if ( 34 != key_z ) break;
      _key_ship(key_y, who_u);
      per_u = eng_peer_make(eng, who_u);
      flo_u = eng_flow_make(eng, per_u, _get_be(key_y + 17),
                            (eng_dire)key_y[25]);

      msg_u = c3_calloc(sizeof(*msg_u));
      msg_u->seq_d = _get_be(key_y + 26);
      msg_u->msg   = mesa_ref(val);
      end_u = &flo_u->lod_u;
      while ( *end_u ) end_u = &(*end_u)->nex_u;
      *end_u = msg_u;
    } break;

    case KEY_CORK: {
      if ( 26 != key_z ) break;
      _key_ship(key_y, who_u);
      per_u = eng_peer_make(eng, who_u);
      eng_cork_set_insert(&per_u->cor_u,
                          (_get_be(key_y + 17) << 1) | (key_y[25] & 1));
    } break;

    default: break;
  }

  mesa_unref(val);
  return 0;
}

static c3_o
_load_state(mesa_engine* eng)
{
  _load_ctx ctx_u = { .eng = eng, .bad_o = c3n };
  c3_y      pre_y[1];

  //  peers first, so flows find their keys; everything else after
  //
  pre_y[0] = KEY_PEER;
  if ( eng->sto_u->scan(eng->sto_u->ctx, pre_y, 1, _load_row, &ctx_u) ) {
    if ( c3y == ctx_u.bad_o ) return c3n;
  }
  for ( c3_y typ_y = 'A'; typ_y <= 'Z'; typ_y++ ) {
    if ( (KEY_PEER == typ_y) || (KEY_IDENT == typ_y) ) continue;
    pre_y[0] = typ_y;
    eng->sto_u->scan(eng->sto_u->ctx, pre_y, 1, _load_row, &ctx_u);
    if ( c3y == ctx_u.bad_o ) return c3n;
  }
  return c3y;
}

static void
_free_state(mesa_engine* eng)
{
  for ( eng_peer_map_itr itr_u = eng_peer_map_first(&eng->per_u);
        !eng_peer_map_is_end(itr_u);
        itr_u = eng_peer_map_next(itr_u) )
  {
    eng_peer_free(itr_u.data->val);
  }
  eng_peer_map_clear(&eng->per_u);
  eng_cache_free(eng);
}

/* eng_reload(): rebuild memory from the store (after a rollback)
*/
void
eng_reload(mesa_engine* eng)
{
  c3_z       len_z;
  eng_peer** per_u;

  _free_state(eng);
  if ( c3n == _load_state(eng) ) {
    eng_txn_fail(eng);
    return;
  }

  //  restart outstanding requests, in their own transaction
  //
  if ( c3n == eng_txn_open(eng) ) {
    return;
  }
  per_u = _peers(eng, &len_z);
  for ( c3_z i = 0; i < len_z; i++ ) {
    eng_flow_resume(eng, per_u[i]);
  }
  c3_free(per_u);
  eng_txn_close(eng);
}

/* the identity record: [[ship rift life ring] ping-bone=(unit @)]
*/
static void
_save_ident(mesa_engine* eng)
{
  c3_y key_y[40];
  c3_y who_y[16];
  c3_y rin_y[65];

  u3_ship_to_bytes(eng->our_u, 16, who_y);
  rin_y[0] = 'B';
  memcpy(rin_y + 1, eng->key_u.sed_y, 64);

  eng_put(eng, key_y, eng_key(KEY_IDENT, eng->our_u, 0, 0, 0, key_y),
          mesa_cell(mesa_qual(mesa_atom_bytes(16, who_y),
                              mesa_atom_u64(eng->rif_d),
                              mesa_atom_u64(eng->lyf_d),
                              mesa_atom_bytes(65, rin_y)),
                    ( c3y == eng->pin_o )
                    ? mesa_cell(mesa_atom_u64(0), mesa_atom_u64(eng->pib_d))
                    : mesa_atom_u64(0)));
}

static int
_any_row(void* arg, const uint8_t* key_y, size_t key_z,
         const uint8_t* val_y, size_t val_z)
{
  *(c3_o*)arg = c3y;
  return 1;
}

static int
_del_row(void* arg, const uint8_t* key_y, size_t key_z,
         const uint8_t* val_y, size_t val_z)
{
  mesa_engine* eng = arg;

  if ( KEY_IDENT != key_y[0] ) {
    if ( eng->sto_u->del(eng->sto_u->ctx, key_y, key_z) ) {
      eng_txn_fail(eng);
      return 1;
    }
  }
  return 0;
}

/* _boot(): check the store against our identity
*/
static mesa_err
_boot(mesa_engine* eng, const mesa_config* cfg)
{
  mesa_store* sto_u = eng->sto_u;
  c3_y        key_y[40];
  c3_z        key_z = eng_key(KEY_IDENT, eng->our_u, 0, 0, 0, key_y);
  c3_y*       val_y;
  c3_z        val_z;
  c3_i        ret_i;
  c3_o        pin_o = c3n;
  c3_d        pib_d = 0;
  c3_o        wip_o = c3n;

  ret_i = sto_u->get(sto_u->ctx, key_y, key_z, &val_y, &val_z);
  if ( ret_i < 0 ) {
    return MESA_E_STORE;
  }

  if ( 1 == ret_i ) {
    //  no identity: an empty store is a fresh start
    //
    c3_o any_o = c3n;
    sto_u->scan(sto_u->ctx, (const c3_y*)"", 0, _any_row, &any_o);
    if ( c3y == any_o ) {
      return MESA_E_IDENTITY;
    }
    if (  !cfg->breached
       && (c3__pawn != u3_ship_rank(eng->our_u)) )
    {
      return MESA_E_FRESH;
    }
  }
  else {
    mesa_noun ide = mesa_cue_bytes(val_z, val_y);
    c3_y      who_y[16];
    u3_ship   who_u;
    c3_d      rif_d, lyf_d;

    c3_free(val_y);
    if (  (MESA_NONE == ide)
       || !mesa_is_atom(mesa_at(4, ide))
       || (mesa_met3(mesa_at(4, ide)) > 16) )
    {
      if ( MESA_NONE != ide ) mesa_unref(ide);
      return MESA_E_STORE;
    }
    mesa_atom_get_bytes(mesa_at(4, ide), 16, who_y);
    u3_ship_of_bytes(who_u, 16, who_y);
    rif_d = _u64(mesa_at(10, ide));
    lyf_d = _u64(mesa_at(22, ide));
    if ( mesa_is_cell(mesa_at(3, ide)) ) {
      pin_o = c3y;
      pib_d = _u64(mesa_at(7, ide));
    }
    mesa_unref(ide);

    if ( c3n == u3_ships_equal(who_u, eng->our_u) ) {
      return MESA_E_IDENTITY;
    }
    if ( (eng->rif_d < rif_d) || (eng->lyf_d < lyf_d) ) {
      return MESA_E_IDENTITY;
    }
    if ( eng->rif_d > rif_d ) {
      //  we breached since we last ran: peers have forgotten us
      //
      if ( !cfg->breached ) {
        return MESA_E_FRESH;
      }
      wip_o = c3y;
      pin_o = c3n;
    }
  }

  if ( c3n == eng_txn_open(eng) ) {
    return MESA_E_STORE;
  }
  if ( c3y == wip_o ) {
    sto_u->scan(sto_u->ctx, (const c3_y*)"", 0, _del_row, eng);
  }
  eng->pin_o = pin_o;
  eng->pib_d = pib_d;
  _save_ident(eng);
  if ( c3n == eng_txn_close(eng) ) {
    return MESA_E_STORE;
  }
  return MESA_OK;
}

/* ping: keep our galaxy's route to us fresh (/app/ping)
*/
static void
_ping(mesa_engine* eng)
{
  c3_y      gal_y;
  u3_ship   gal_u = {0};
  eng_peer* per_u;
  eng_flow* flo_u;

  if ( (c3y == eng->pif_o) || (eng->now_d < eng->pin_d) ) {
    return;
  }
  if ( c3n == eng_ship_galaxy(eng, eng->our_u, &gal_y) ) {
    eng->pin_d = eng->now_d + ENG_PING_MS;
    return;
  }
  gal_u[0] = gal_y;

  //  our galaxy changed (or this is the first ping): new flow
  //
  if ( (c3n == eng->pin_o) || (c3n == u3_ships_equal(gal_u, eng->gal_u)) ) {
    per_u = eng_peer_make(eng, gal_u);
    eng->pib_d = per_u->bon_d;
    per_u->bon_d += 4;
    eng_peer_save(eng, per_u);
    eng->pin_o = c3y;
    u3_ship_copy(eng->gal_u, gal_u);
    _save_ident(eng);
  }

  per_u = eng_peer_make(eng, gal_u);
  flo_u = eng_flow_make(eng, per_u, eng->pib_d, DIRE_FOR);

  //  one at a time
  //
  if ( flo_u->lod_u ) {
    eng->pif_o = c3y;
    if ( c3n == per_u->kno_o ) eng_peer_ask(eng, per_u);
    return;
  }

  //  [%plea %g /ge/ping [%0 %m %noun ~]]
  //
  eng->pif_o = c3y;
  if ( c3n == per_u->kno_o ) {
    eng_peer_ask(eng, per_u);
  }
  eng_flow_pump(eng, per_u, flo_u,
    mesa_qual(mesa_atom_cord("plea"),
              mesa_atom_cord("g"),
              mesa_cell(mesa_atom_cord("ge"),
                        mesa_cell(mesa_atom_cord("ping"), mesa_atom_u64(0))),
              mesa_trel(mesa_atom_u64(0),
                        mesa_atom_cord("m"),
                        mesa_cell(mesa_atom_cord("noun"), mesa_atom_u64(0)))));
}

/* lifecycle
*/
mesa_err
mesa_engine_open(const mesa_config* cfg, mesa_engine** out)
{
  mesa_engine* eng;
  mesa_err     err_e;
  mesa_ship    who = cfg->ident.who;

  if ( !cfg->store || ('B' != cfg->ident.ring[0]) ) {
    return MESA_E_ARG;
  }

  eng = c3_calloc(sizeof(*eng));
  eng->hok_u = cfg->hooks;
  eng->sto_u = cfg->store;
  eng->our_u[0] = who.lo;
  eng->our_u[1] = who.hi;
  eng->rif_d = ( MESA_RIFT_UNKNOWN == cfg->ident.rift ) ? 0 : cfg->ident.rift;
  eng->lyf_d = cfg->ident.life;
  eng->now_d = cfg->now;
  eng->ont_u = &eng->out_u;
  eng->bad_o = c3n;
  eng->rel_o = c3n;
  eng->pif_o = c3n;
  eng->pin_o = c3n;
  eng->app_o = c3n;
  eng->hok_o = c3n;
  eng->pin_d = cfg->now;
  eng_peer_map_init(&eng->per_u);

  mesa_keys_from_ring(&eng->key_u, 65, cfg->ident.ring);
  mesa_keys_pass(&eng->key_u, eng->pas_y);

  //  our sponsor: fixed for galaxies, comets and moons
  //
  switch ( u3_ship_rank(eng->our_u) ) {
    case c3__czar:
    case c3__pawn:
    case c3__earl: u3_ship_sein(eng->spo_u, eng->our_u); break;
    default: {
      eng->spo_u[0] = cfg->our_sponsor.lo;
      eng->spo_u[1] = cfg->our_sponsor.hi;
      if ( !cfg->our_sponsor.lo && !cfg->our_sponsor.hi ) {
        u3_ship_sein(eng->spo_u, eng->our_u);
      }
    }
  }

  if ( MESA_OK != (err_e = _boot(eng, cfg)) ) {
    _free_state(eng);
    eng_peer_map_cleanup(&eng->per_u);
    c3_free(eng);
    return err_e;
  }

  if ( (c3__czar == u3_ship_rank(eng->our_u)) || cfg->no_ping ) {
    eng->pin_d = UINT64_MAX;
  }

  if ( c3n == _load_state(eng) ) {
    _free_state(eng);
    eng_peer_map_cleanup(&eng->per_u);
    c3_free(eng);
    return MESA_E_STORE;
  }

  //  the galaxy we ping, as last saved
  //
  if ( c3y == eng->pin_o ) {
    c3_y gal_y;
    if ( c3y == eng_ship_galaxy(eng, eng->our_u, &gal_y) ) {
      memset(eng->gal_u, 0, sizeof(u3_ship));
      eng->gal_u[0] = gal_y;
    }
  }

  //  restart whatever was in flight
  //
  if ( c3y == eng_txn_open(eng) ) {
    c3_z       len_z;
    eng_peer** per_u = _peers(eng, &len_z);

    for ( c3_z i = 0; i < len_z; i++ ) {
      eng_flow_resume(eng, per_u[i]);
    }
    c3_free(per_u);
    eng_txn_close(eng);
  }

  *out = eng;
  return MESA_OK;
}

void
mesa_engine_close(mesa_engine* eng)
{
  if ( !eng ) return;
  if ( eng->txn_w ) {
    eng->sto_u->rollback(eng->sto_u->ctx);
  }
  eng_out_cut(eng, &eng->out_u);
  _free_state(eng);
  eng_peer_map_cleanup(&eng->per_u);
  c3_free(eng);
}

mesa_ship
mesa_engine_ship(mesa_engine* eng)
{
  return (mesa_ship){ .lo = eng->our_u[0], .hi = eng->our_u[1] };
}

/* inputs
*/
static void
_tick(mesa_engine* eng, c3_d now_d)
{
  if ( now_d > eng->now_d ) {
    eng->now_d = now_d;
  }
}

void
mesa_engine_hear(mesa_engine* eng, mesa_lane from,
                 const uint8_t* buf, size_t len, uint64_t now)
{
  u3_mesa_pact pac_u;
  c3_y         buf_y[PACT_SIZE];

  _tick(eng, now);

  if (  (c3y == eng->bad_o) || (len > PACT_SIZE)
     || (c3n == mesa_is_new_pact((c3_y*)buf, (c3_h)len)) )
  {
    return;
  }

  memcpy(buf_y, buf, len);
  memset(&pac_u, 0, sizeof(pac_u));
  if ( mesa_sift_pact_from_buf(&pac_u, buf_y, (c3_h)len) ) {
    return;
  }

  if ( c3n == eng_txn_open(eng) ) {
    return;
  }

  switch ( pac_u.hed_u.typ_y ) {
    case PACT_PEEK: {
      if ( c3y == u3_ships_equal(pac_u.pek_u.nam_u.her_u, eng->our_u) ) {
        eng_serve_peek(eng, from, &pac_u.pek_u.nam_u);
      }
    } break;

    case PACT_PAGE: {
      eng_hear_page(eng, from, &pac_u);
    } break;

    case PACT_POKE: {
      eng_hear_poke(eng, from, &pac_u);
    } break;

    default: break;
  }

  eng_txn_close(eng);
}

void
mesa_engine_wake(mesa_engine* eng, uint64_t now)
{
  c3_z       len_z;
  eng_peer** per_u;

  _tick(eng, now);

  if ( c3n == eng_txn_open(eng) ) {
    return;
  }

  per_u = _peers(eng, &len_z);
  for ( c3_z i = 0; i < len_z; i++ ) {
    eng_peer* p = per_u[i];

    eng_req_wake(eng, p);

    //  refresh keys of peers we're talking to
    //
    if (  (c3y == p->kno_o) && p->req_u
       && (c3__pawn != u3_ship_rank(p->who_u))
       && (eng->now_d >= p->ask_d + ENG_POINT_MS) )
    {
      eng_peer_ask(eng, p);
    }
  }
  c3_free(per_u);

  _ping(eng);
  eng_txn_close(eng);
}

uint64_t
mesa_engine_deadline(mesa_engine* eng)
{
  c3_d dea_d = eng->pin_d;

  if ( c3y == eng->pif_o ) {
    dea_d = UINT64_MAX;
  }
  for ( eng_peer_map_itr itr_u = eng_peer_map_first(&eng->per_u);
        !eng_peer_map_is_end(itr_u);
        itr_u = eng_peer_map_next(itr_u) )
  {
    dea_d = c3_min(dea_d, eng_req_deadline(itr_u.data->val));
  }
  return dea_d;
}

/* keys
*/
void
mesa_engine_point(mesa_engine* eng, const mesa_point* pot)
{
  u3_ship   who_u = { pot->who.lo, pot->who.hi };
  eng_peer* per_u;

  if ( (c3y == u3_ships_equal(who_u, eng->our_u)) || (c3n == eng_txn_open(eng)) ) {
    return;
  }

  per_u = eng_peer_make(eng, who_u);
  per_u->ask_o = c3n;
  per_u->ask_d = eng->now_d;

  if ( pot->has_sponsor ) {
    per_u->spo_u[0] = pot->sponsor.lo;
    per_u->spo_u[1] = pot->sponsor.hi;
    per_u->spo_o = c3y;
  }

  //  no networking keys: nothing to do yet
  //
  if ( !pot->life || ('b' != pot->pass[0]) ) {
    eng_peer_save(eng, per_u);
    eng_txn_close(eng);
    return;
  }

  if ( c3y == per_u->kno_o ) {
    if ( pot->rift > per_u->rif_d ) {
      mesa_ship her = { .lo = who_u[0], .hi = who_u[1] };

      eng_peer_breach(eng, per_u);
      eng_peer_set_keys(eng, per_u, pot->rift, pot->life, pot->pass);
      eng_peer_save(eng, per_u);
      if ( eng->hok_u.breach ) {
        eng->hok_u.breach(eng->hok_u.ctx, her);
      }
    }
    else if ( pot->life != per_u->lyf_d ) {
      eng_peer_set_keys(eng, per_u, per_u->rif_d, pot->life, pot->pass);
      eng_peer_save(eng, per_u);
      eng_req_rebase(eng, per_u);
      eng_flow_resume(eng, per_u);
    }
    else {
      eng_peer_save(eng, per_u);
    }
  }
  else {
    eng_peer_set_keys(eng, per_u, pot->rift, pot->life, pot->pass);
    eng_peer_known(eng, per_u);
  }

  eng_txn_close(eng);
}

void
mesa_engine_point_missing(mesa_engine* eng, mesa_ship who)
{
  u3_ship   who_u = { who.lo, who.hi };
  eng_peer* per_u = eng_peer_get(eng, who_u);

  //  we'll ask again later
  //
  if ( per_u ) {
    per_u->ask_d = eng->now_d;
  }
}

/* application transactions
*/
mesa_err
mesa_txn_begin(mesa_engine* eng)
{
  if ( c3y == eng->hok_o ) {
    return MESA_E_STATE;
  }
  if ( c3y == eng->app_o ) {
    return MESA_E_STATE;
  }
  if ( c3n == eng_txn_open(eng) ) {
    return MESA_E_FAILED;
  }
  eng->app_o = c3y;
  return MESA_OK;
}

mesa_err
mesa_txn_commit(mesa_engine* eng)
{
  if ( c3n == eng->app_o ) {
    return MESA_E_STATE;
  }
  eng->app_o = c3n;
  return ( c3y == eng_txn_close(eng) ) ? MESA_OK : MESA_E_FAILED;
}

void
mesa_txn_abort(mesa_engine* eng)
{
  if ( c3n == eng->app_o ) {
    return;
  }
  eng->app_o = c3n;
  eng->txn_w = 0;
  eng->sto_u->rollback(eng->sto_u->ctx);
  eng_out_cut(eng, &eng->out_u);
  eng_reload(eng);
}

/* the flow API
*/
static c3_o
_api_open(mesa_engine* eng)
{
  if ( c3n == eng_txn_open(eng) ) {
    return c3n;
  }
  eng->api_d++;
  return c3y;
}

static mesa_err
_api_close(mesa_engine* eng, mesa_err err_e)
{
  if ( c3n == eng_txn_close(eng) ) {
    return MESA_E_FAILED;
  }
  return err_e;
}

static void
_contact(mesa_engine* eng, eng_peer* per_u)
{
  if ( c3y == per_u->kno_o ) {
    return;
  }
  if ( c3__pawn == u3_ship_rank(per_u->who_u) ) {
    eng_comet_proof(eng, per_u, _lane_none());
  }
  else {
    eng_peer_ask(eng, per_u);
  }
}

mesa_err
mesa_flow_open(mesa_engine* eng, mesa_ship her, uint64_t* bone)
{
  u3_ship   who_u = { her.lo, her.hi };
  eng_peer* per_u;
  eng_flow* flo_u;

  if ( c3y == u3_ships_equal(who_u, eng->our_u) ) {
    return MESA_E_ARG;
  }
  if ( c3n == _api_open(eng) ) {
    return MESA_E_FAILED;
  }

  per_u = eng_peer_make(eng, who_u);
  *bone = per_u->bon_d;
  per_u->bon_d += 4;
  eng_peer_save(eng, per_u);

  flo_u = eng_flow_make(eng, per_u, *bone, DIRE_FOR);
  eng_flow_save(eng, per_u, flo_u);
  _contact(eng, per_u);

  return _api_close(eng, MESA_OK);
}

static mesa_err
_plea(mesa_engine* eng, mesa_ship her, uint64_t bone, mesa_noun msg,
      c3_o cor_o)
{
  u3_ship   who_u = { her.lo, her.hi };
  eng_peer* per_u;
  eng_flow* flo_u;

  if ( c3n == _api_open(eng) ) {
    mesa_unref(msg);
    return MESA_E_FAILED;
  }

  if (  !(per_u = eng_peer_get(eng, who_u))
     || !(flo_u = eng_flow_get(per_u, bone, DIRE_FOR)) )
  {
    mesa_unref(msg);
    return _api_close(eng, MESA_E_ARG);
  }
  if ( c3y == flo_u->clo_o ) {
    mesa_unref(msg);
    return _api_close(eng, MESA_E_STATE);
  }

  if ( c3y == cor_o ) {
    flo_u->clo_o = c3y;
  }
  eng_flow_pump(eng, per_u, flo_u, msg);
  _contact(eng, per_u);

  return _api_close(eng, MESA_OK);
}

mesa_err
mesa_flow_plea(mesa_engine* eng, mesa_ship her, uint64_t bone,
               mesa_noun vane, mesa_noun path, mesa_noun payload)
{
  return _plea(eng, her, bone,
               mesa_qual(mesa_atom_cord("plea"), vane, path, payload), c3n);
}

mesa_err
mesa_flow_cork(mesa_engine* eng, mesa_ship her, uint64_t bone)
{
  //  [%plea %$ /flow %cork ~]
  //
  return _plea(eng, her, bone,
               mesa_qual(mesa_atom_cord("plea"),
                         mesa_atom_u64(0),
                         mesa_cell(mesa_atom_cord("flow"), mesa_atom_u64(0)),
                         mesa_cell(mesa_atom_cord("cork"), mesa_atom_u64(0))),
               c3y);
}

mesa_err
mesa_flow_boon(mesa_engine* eng, mesa_ship her, uint64_t bone,
               mesa_noun payload)
{
  u3_ship   who_u = { her.lo, her.hi };
  eng_peer* per_u;
  eng_flow* flo_u;

  if ( c3n == _api_open(eng) ) {
    mesa_unref(payload);
    return MESA_E_FAILED;
  }

  if (  !(per_u = eng_peer_get(eng, who_u))
     || !(flo_u = eng_flow_get(per_u, bone, DIRE_BAK)) )
  {
    mesa_unref(payload);
    return _api_close(eng, MESA_E_ARG);
  }
  if ( c3y == flo_u->clo_o ) {
    mesa_unref(payload);
    return _api_close(eng, MESA_E_STATE);
  }

  eng_flow_pump(eng, per_u, flo_u,
                mesa_cell(mesa_atom_cord("boon"), payload));
  return _api_close(eng, MESA_OK);
}

mesa_err
mesa_flow_done(mesa_engine* eng, mesa_ship her, uint64_t bone,
               mesa_noun error)
{
  u3_ship   who_u = { her.lo, her.hi };
  eng_peer* per_u;
  eng_flow* flo_u;

  if ( c3n == _api_open(eng) ) {
    if ( MESA_NONE != error ) mesa_unref(error);
    return MESA_E_FAILED;
  }

  if (  !(per_u = eng_peer_get(eng, who_u))
     || !(flo_u = eng_flow_get(per_u, bone, DIRE_BAK))
     || (c3n == flo_u->pen_o) )
  {
    if ( MESA_NONE != error ) mesa_unref(error);
    return _api_close(eng, MESA_E_STATE);
  }

  eng_flow_done(eng, per_u, flo_u, error);
  return _api_close(eng, MESA_OK);
}

/* remote scry
*/
mesa_err
mesa_keen(mesa_engine* eng, mesa_ship her, const char* path)
{
  u3_ship   who_u = { her.lo, her.hi };
  eng_peer* per_u;
  eng_req*  req_u;
  knot_path pax_u;
  c3_c*     pat_c;

  if (  ('/' != path[0])
     || (c3n == knot_path_split(strlen(path), path, &pax_u))
     || (c3y == u3_ships_equal(who_u, eng->our_u)) )
  {
    return MESA_E_ARG;
  }
  if ( c3n == eng_txn_open(eng) ) {
    return MESA_E_FAILED;
  }

  per_u = eng_peer_make(eng, who_u);

  for ( req_u = per_u->req_u; req_u; req_u = req_u->nex_u ) {
    if ( (REQ_KEEN == req_u->typ_e) && !strcmp(req_u->key_c, path) ) {
      return _api_close(eng, MESA_OK);   //  already asked
    }
  }

  pat_c = eng_keen_path(per_u, path);
  req_u = eng_req_add(eng, per_u, pat_c, REQ_KEEN);
  req_u->key_c = strdup(path);
  c3_free(pat_c);

  _contact(eng, per_u);
  eng_req_send(eng, per_u, req_u);
  return _api_close(eng, MESA_OK);
}

mesa_err
mesa_yawn(mesa_engine* eng, mesa_ship her, const char* path)
{
  u3_ship   who_u = { her.lo, her.hi };
  eng_peer* per_u = eng_peer_get(eng, who_u);

  if ( !per_u ) {
    return MESA_OK;
  }
  for ( eng_req* req_u = per_u->req_u; req_u; req_u = req_u->nex_u ) {
    if ( (REQ_KEEN == req_u->typ_e) && !strcmp(req_u->key_c, path) ) {
      eng_req_del(per_u, req_u);
      eng_req_free(req_u);
      break;
    }
  }
  return MESA_OK;
}

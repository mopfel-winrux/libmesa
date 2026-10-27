/// @file
///
/// Peers: keys (+sy-publ), routes (+make-lanes, +ev-update-lane) and
/// comet attestation (|al in ames.hoon).

#include "eng.h"
#include "mesa/ident.h"
#include "urcrypt.h"

static u3_ship_key
_key(u3_ship who_u)
{
  return (u3_ship_key){ .d = { who_u[0], who_u[1] } };
}

/* eng_peer_get(): [who_u] from memory, or from the store if we unloaded
**                 them (eng_peer_sweep()); 0 if we've never heard of them
*/
eng_peer*
eng_peer_get(mesa_engine* eng, u3_ship who_u)
{
  eng_peer_map_itr itr_u = eng_peer_map_get(&eng->per_u, _key(who_u));
  eng_peer*        per_u;

  if ( !eng_peer_map_is_end(itr_u) ) {
    per_u = itr_u.data->val;
  }
  else if ( (c3y == eng->lod_o) || !(per_u = eng_peer_load(eng, who_u)) ) {
    return 0;
  }
  per_u->use_d = eng->now_d;
  return per_u;
}

eng_peer*
eng_peer_new(mesa_engine* eng, u3_ship who_u)
{
  eng_peer* per_u = c3_calloc(sizeof(*per_u));

  u3_ship_copy(per_u->who_u, who_u);
  per_u->kno_o = c3n;
  per_u->ask_o = c3n;
  per_u->spo_o = c3n;
  per_u->lan_o = c3n;
  per_u->liv_o = c3n;
  per_u->gfo_o = c3n;
  per_u->gbo_o = c3n;
  per_u->jao_o = c3n;
  per_u->moa_o = c3n;
  per_u->aho_o = c3n;
  per_u->use_d = eng->now_d;
  eng_flow_map_init(&per_u->flo_u);
  eng_req_map_init(&per_u->rem_u);

  //  congestion: vere's _init_gage, in ms
  //
  per_u->rto_d = 200;
  per_u->rtt_d = 1000;
  per_u->rtv_d = 1000;
  per_u->wnd_d = 1;
  per_u->sst_d = 10000;

  //  galaxies are always reachable at their own lane
  //
  if ( c3__czar == u3_ship_rank(who_u) ) {
    per_u->lan_o = c3y;
    per_u->hop_y = 0;
    per_u->lan_u = (mesa_lane){ .kind = MESA_LANE_GALAXY,
                                .galaxy = (c3_y)who_u[0] };
  }

  if ( eng_peer_map_is_end(eng_peer_map_insert(&eng->per_u, _key(who_u), per_u)) ) {
    mesa_fail(__FILE__, __LINE__, "out of memory");
  }
  eng->ali_w++;
  return per_u;
}

/* eng_peer_evict(): forget a stranger we have nothing going on with
*/
c3_o
eng_peer_evict(mesa_engine* eng)
{
  eng_peer* old_u = 0;

  for ( eng_peer_map_itr itr_u = eng_peer_map_first(&eng->per_u);
        !eng_peer_map_is_end(itr_u);
        itr_u = eng_peer_map_next(itr_u) )
  {
    eng_peer* per_u = itr_u.data->val;

    if (  (c3n == per_u->kno_o)
       && !per_u->req_u
       && !eng_flow_map_size(&per_u->flo_u)
       && (eng->now_d >= per_u->ask_d + 60000) )
    {
      old_u = per_u;
      break;
    }
  }

  if ( !old_u ) {
    return c3n;
  }
  eng_peer_map_erase(&eng->per_u, _key(old_u->who_u));
  eng_peer_free(old_u);
  eng->ali_w--;
  return c3y;
}

/* _peer_idle(): could [per_u] leave memory? Everything we keep for them
**               is in the store; what's lost (their lane, rtt) we learn
**               again. Not while anything of theirs runs on a timer.
*/
static c3_o
_peer_idle(mesa_engine* eng, eng_peer* per_u)
{
  if (  (c3__czar == u3_ship_rank(per_u->who_u))
     || (c3y == u3_ships_equal(per_u->who_u, eng->spo_u))
     || per_u->req_u
     || (c3y == per_u->ask_o)
     || (eng->now_d < per_u->use_d + ENG_IDLE_MS)
     || (eng->now_d < per_u->con_d + ENG_IDLE_MS) )
  {
    return c3n;
  }
  for ( eng_flow_map_itr itr_u = eng_flow_map_first(&per_u->flo_u);
        !eng_flow_map_is_end(itr_u);
        itr_u = eng_flow_map_next(itr_u) )
  {
    eng_flow* flo_u = itr_u.data->val;
    if (  flo_u->lod_u || (c3y == flo_u->hat_o)
       || (c3y == flo_u->clo_o) || (c3y == flo_u->pen_o) )
    {
      return c3n;
    }
  }
  return c3y;
}

/* _peer_moot(): is there nothing to remember about [per_u] but keys,
**               which azimuth (or their attestation) gives again? Never
**               once we've opened a flow to them: reusing its bone would
**               look like a corked flow to them.
*/
static c3_o
_peer_moot(eng_peer* per_u)
{
  return __(  (0 == per_u->bon_d)
           && !eng_flow_map_size(&per_u->flo_u)
           && !eng_cork_count(per_u)
           && (c3n == per_u->gfo_o) && (c3n == per_u->gbo_o)
           && (c3n == per_u->jao_o) && (c3n == per_u->aho_o)
           && (c3n == per_u->moa_o) );
}

/* eng_peer_sweep(): unload quiet peers; forget the ones there's nothing
**                   to remember about
*/
void
eng_peer_sweep(mesa_engine* eng)
{
  eng_peer** out_u = 0;
  c3_w       len_w = 0, cap_w = 0;

  if ( eng->now_d < eng->gcd_d ) {
    return;
  }
  eng->gcd_d = eng->now_d + ENG_GC_MS;

  for ( eng_peer_map_itr itr_u = eng_peer_map_first(&eng->per_u);
        !eng_peer_map_is_end(itr_u);
        itr_u = eng_peer_map_next(itr_u) )
  {
    eng_peer* per_u = itr_u.data->val;
    if ( c3y == _peer_idle(eng, per_u) ) {
      if ( len_w == cap_w ) {
        cap_w = c3_max(16, cap_w * 2);
        out_u = c3_realloc(out_u, cap_w * sizeof(eng_peer*));
      }
      out_u[len_w++] = per_u;
    }
  }

  for ( c3_w i = 0; i < len_w; i++ ) {
    eng_peer* per_u = out_u[i];

    c3_y key_y[40];
    c3_z key_z = eng_key(KEY_ROUT, per_u->who_u, 0, 0, 0, key_y);

    if ( c3y == _peer_moot(per_u) ) {
      eng_del(eng, key_y, key_z);
      eng_del(eng, key_y, eng_key(KEY_PEER, per_u->who_u, 0, 0, 0, key_y));
    }
    else if ( (c3y == per_u->lan_o) && (MESA_LANE_IP == per_u->lan_u.kind) ) {
      //  where they were, to try first when they're back
      //
      eng_put(eng, key_y, key_z,
              mesa_cell(mesa_atom_u64(per_u->lan_u.ip),
                        mesa_atom_u64(per_u->lan_u.port)));
    }
    if ( c3n == per_u->kno_o ) {
      eng->ali_w--;
    }
    eng_peer_map_erase(&eng->per_u, _key(per_u->who_u));
    eng_peer_free(per_u);
  }
  c3_free(out_u);
}

eng_peer*
eng_peer_make(mesa_engine* eng, u3_ship who_u)
{
  eng_peer* per_u = eng_peer_get(eng, who_u);
  return per_u ? per_u : eng_peer_new(eng, who_u);
}

void
eng_peer_free(eng_peer* per_u)
{
  for ( eng_flow_map_itr itr_u = eng_flow_map_first(&per_u->flo_u);
        !eng_flow_map_is_end(itr_u);
        itr_u = eng_flow_map_next(itr_u) )
  {
    eng_flow_free(itr_u.data->val);
  }
  eng_flow_map_cleanup(&per_u->flo_u);
  for ( c3_w i = 0; i < 4; i++ ) {
    c3_free(per_u->crk_u[i].ran_u);
  }

  eng_req_clear(per_u);
  eng_req_map_cleanup(&per_u->rem_u);
  while ( per_u->stl_u ) {
    eng_out* nex_u = per_u->stl_u->nex_u;
    c3_free(per_u->stl_u->buf_y);
    c3_free(per_u->stl_u);
    per_u->stl_u = nex_u;
  }
  c3_free(per_u);
}

static mesa_noun
_unit(c3_o has_o, c3_d val_d)
{
  return ( c3y == has_o )
         ? mesa_cell(mesa_atom_u64(0), mesa_atom_u64(val_d))
         : mesa_atom_u64(0);
}

/* eng_peer_save(): persist
**
**   [known [rift life pass] sponsor=(unit)
**    [next-bone gf=(unit) jael=(unit) moon-asked=? ahoy=(unit bone)]]
*/
void
eng_peer_save(mesa_engine* eng, eng_peer* per_u)
{
  c3_y      key_y[40];
  c3_z      key_z = eng_key(KEY_PEER, per_u->who_u, 0, 0, 0, key_y);
  c3_y      pas_y[65];
  c3_y      spo_y[16];
  mesa_noun spo;

  pas_y[0] = 'b';
  memcpy(pas_y + 1,  per_u->sgn_y, 32);
  memcpy(pas_y + 33, per_u->cry_y, 32);

  if ( c3y == per_u->spo_o ) {
    u3_ship_to_bytes(per_u->spo_u, 16, spo_y);
    spo = mesa_cell(mesa_atom_u64(0), mesa_atom_bytes(16, spo_y));
  }
  else {
    spo = mesa_atom_u64(0);
  }

  eng_put(eng, key_y, key_z,
          mesa_qual(mesa_atom_u64(c3y == per_u->kno_o),
                    mesa_trel(mesa_atom_u64(per_u->rif_d),
                              mesa_atom_u64(per_u->lyf_d),
                              mesa_atom_bytes(65, pas_y)),
                    spo,
                    mesa_qual(mesa_atom_u64(per_u->bon_d),
                              _unit(per_u->gfo_o, per_u->gfb_d),
                              _unit(per_u->jao_o, per_u->jab_d),
                              mesa_cell(mesa_atom_u64(c3y == per_u->moa_o),
                                        _unit(per_u->aho_o, per_u->ahb_d)))));
}

/* eng_peer_ask(): request [per_u]'s point from the host
*/
void
eng_peer_ask(mesa_engine* eng, eng_peer* per_u)
{
  mesa_ship who = { .lo = per_u->who_u[0], .hi = per_u->who_u[1] };

  if ( (c3y == per_u->ask_o) && (eng->now_d < per_u->ask_d + 30000) ) {
    return;
  }
  per_u->ask_o = c3y;
  per_u->ask_d = eng->now_d;

  //  comets attest for themselves
  //
  if ( c3__pawn == u3_ship_rank(per_u->who_u) ) {
    if ( c3n == per_u->kno_o ) {
      eng_comet_proof(eng, per_u, (mesa_lane){ .kind = MESA_LANE_NONE });
    }
    return;
  }

  //  moons' parents speak for them
  //
  if ( c3y == eng_sys_moon(eng, per_u) ) {
    return;
  }

  if ( eng->hok_u.need_point ) {
    eng->hok_u.need_point(eng->hok_u.ctx, who);
  }
}

/* _sponsor(): next ship up [who_u]'s sponsorship chain
*/
static void
_sponsor(mesa_engine* eng, u3_ship who_u, u3_ship out_u)
{
  eng_peer* per_u;

  if ( c3y == u3_ships_equal(who_u, eng->our_u) ) {
    u3_ship_copy(out_u, eng->spo_u);
    return;
  }
  per_u = eng_peer_get(eng, who_u);
  if ( per_u && (c3y == per_u->spo_o) ) {
    u3_ship_copy(out_u, per_u->spo_u);
    return;
  }
  u3_ship_sein(out_u, who_u);
}

/* eng_ship_galaxy(): the galaxy at the top of [who_u]'s chain (+saxo)
**
**   Uses sponsors from azimuth where we have them, +sein otherwise, and
**   asks for points we're missing so escaped ships route correctly.
*/
c3_o
eng_ship_galaxy(mesa_engine* eng, u3_ship who_u, c3_y* gal_y)
{
  u3_ship cur_u;

  u3_ship_copy(cur_u, who_u);

  for ( c3_w i = 0; i < 8; i++ ) {
    u3_ship   nex_u;
    eng_peer* per_u;
    c3_h      ran_h = u3_ship_rank(cur_u);

    if ( c3__czar == ran_h ) {
      *gal_y = (c3_y)cur_u[0];
      return c3y;
    }

    //  stars and planets other than us: learn their real sponsor
    //
    if (  ((c3__king == ran_h) || (c3__duke == ran_h))
       && (c3n == u3_ships_equal(cur_u, eng->our_u)) )
    {
      per_u = eng_peer_make(eng, cur_u);
      if ( c3n == per_u->spo_o ) {
        eng_peer_ask(eng, per_u);
      }
    }

    _sponsor(eng, cur_u, nex_u);
    if ( c3y == u3_ships_equal(nex_u, cur_u) ) {
      return c3n;
    }
    u3_ship_copy(cur_u, nex_u);
  }
  return c3n;
}

c3_o
eng_peer_galaxy(mesa_engine* eng, eng_peer* per_u, c3_y* gal_y)
{
  return eng_ship_galaxy(eng, per_u->who_u, gal_y);
}

static c3_o
_peer_dead(mesa_engine* eng, eng_peer* per_u)
{
  if ( c3__czar == u3_ship_rank(per_u->who_u) ) {
    return c3n;
  }
  return __( (c3n == per_u->liv_o) || (eng->now_d >= per_u->con_d + ENG_DEAD_MS) );
}

/* eng_peer_lanes(): +make-lanes; at most two lanes
*/
c3_w
eng_peer_lanes(mesa_engine* eng, eng_peer* per_u, mesa_lane las_u[2])
{
  c3_y gal_y;
  c3_o gal_o;
  c3_w len_w = 0;

  if ( c3__czar == u3_ship_rank(per_u->who_u) ) {
    las_u[0] = (mesa_lane){ .kind = MESA_LANE_GALAXY,
                            .galaxy = (c3_y)per_u->who_u[0] };
    return 1;
  }

  gal_o = eng_peer_galaxy(eng, per_u, &gal_y);

  //  never route through ourselves
  //
  if (  (c3y == gal_o)
     && (c3__czar == u3_ship_rank(eng->our_u))
     && ((c3_y)eng->our_u[0] == gal_y) )
  {
    gal_o = c3n;
  }

  if ( c3y == per_u->lan_o ) {
    las_u[len_w++] = per_u->lan_u;

    //  direct and alive: that's enough
    //
    if ( (0 == per_u->hop_y) && (c3n == _peer_dead(eng, per_u)) ) {
      return len_w;
    }
  }

  if ( c3y == gal_o ) {
    mesa_lane gal_u = { .kind = MESA_LANE_GALAXY, .galaxy = gal_y };
    if ( !len_w || (c3n == eng_lanes_eq(las_u[0], gal_u)) ) {
      las_u[len_w++] = gal_u;
    }
  }
  return len_w;
}

/* eng_peer_heard(): +ev-update-lane and qos
*/
void
eng_peer_heard(mesa_engine* eng, eng_peer* per_u,
               mesa_lane lan_u, c3_y hop_y, mesa_lane* nex_u)
{
  per_u->con_d = eng->now_d;
  per_u->liv_o = c3y;

  if ( c3__czar == u3_ship_rank(per_u->who_u) ) {
    return;
  }

  if ( 0 == hop_y ) {
    per_u->lan_o = c3y;
    per_u->hop_y = 0;
    per_u->lan_u = lan_u;
    return;
  }

  if ( !nex_u ) {
    return;
  }

  //  indirect: keep a direct route to the same lane, else take it
  //
  if (  (c3y == per_u->lan_o)
     && (0 == per_u->hop_y)
     && (c3y == eng_lanes_eq(per_u->lan_u, *nex_u)) )
  {
    return;
  }
  per_u->lan_o = c3y;
  per_u->hop_y = hop_y;
  per_u->lan_u = *nex_u;
}

/* eng_peer_set_keys(): install azimuth keys; derive the symmetric key
*/
void
eng_peer_set_keys(mesa_engine* eng, eng_peer* per_u,
                  c3_d rif_d, c3_d lyf_d, const c3_y pas_y[65])
{
  per_u->rif_d = rif_d;
  per_u->lyf_d = lyf_d;
  memcpy(per_u->sgn_y, pas_y + 1,  32);
  memcpy(per_u->cry_y, pas_y + 33, 32);
  mesa_symmetric_key(per_u->cry_y, &eng->key_u, per_u->sym_y);
}

/* eng_peer_breach(): +on-publ-breach; forget everything but the keys
*/
void
eng_peer_breach(mesa_engine* eng, eng_peer* per_u)
{
  c3_y key_y[40];

  for ( eng_flow_map_itr itr_u = eng_flow_map_first(&per_u->flo_u);
        !eng_flow_map_is_end(itr_u);
        itr_u = eng_flow_map_next(itr_u) )
  {
    eng_flow* flo_u = itr_u.data->val;

    for ( eng_msg* msg_u = flo_u->lod_u; msg_u; msg_u = msg_u->nex_u ) {
      c3_z key_z = eng_key(KEY_LOAD, per_u->who_u, flo_u->bon_d,
                           flo_u->dir_e, msg_u->seq_d, key_y);
      eng_del(eng, key_y, key_z);
    }
    eng_del(eng, key_y, eng_key(KEY_FLOW, per_u->who_u, flo_u->bon_d,
                                flo_u->dir_e, 0, key_y));
    eng_flow_free(flo_u);
  }
  eng_flow_map_clear(&per_u->flo_u);

  eng_cork_wipe(eng, per_u);

  for ( eng_req* req_u = per_u->req_u; req_u; req_u = req_u->nex_u ) {
    if ( (REQ_KEEN == req_u->typ_e) && eng->hok_u.sage ) {
      mesa_ship her = { .lo = per_u->who_u[0], .hi = per_u->who_u[1] };
      eng->hok_u.sage(eng->hok_u.ctx, her, req_u->key_c, MESA_NONE);
    }
  }
  eng_req_clear(per_u);

  eng_sys_breach(eng, per_u);
  per_u->bon_d = 0;
  per_u->lan_o = c3n;
}

/* eng_peer_stash(): keep the datagram we're hearing from [per_u], whose
**                   keys we don't have yet, to hear once we do (the
**                   first few)
*/
void
eng_peer_stash(mesa_engine* eng, eng_peer* per_u, mesa_lane lan_u)
{
  eng_out*  out_u;
  eng_out** end_u = &per_u->stl_u;

  if ( !eng->hea_y || (per_u->stn_w >= ENG_STASH_MAX) ) return;
  for ( ; *end_u; end_u = &(*end_u)->nex_u ) {
    if (  ((*end_u)->len_z == eng->hea_z)
       && !memcmp((*end_u)->buf_y, eng->hea_y, eng->hea_z) )
    {
      return;   //  a resend
    }
  }
  out_u = c3_calloc(sizeof(*out_u));
  out_u->lan_u = lan_u;
  out_u->len_z = eng->hea_z;
  out_u->buf_y = c3_malloc(eng->hea_z);
  memcpy(out_u->buf_y, eng->hea_y, eng->hea_z);
  *end_u = out_u;
  per_u->stn_w++;
}

/* eng_peer_known(): keys arrived; start anything that was waiting
*/
void
eng_peer_known(mesa_engine* eng, eng_peer* per_u)
{
  if ( (c3n == per_u->kno_o) && eng->ali_w ) {
    eng->ali_w--;
  }
  per_u->kno_o = c3y;
  per_u->ask_o = c3n;
  eng_peer_save(eng, per_u);
  eng_req_rebase(eng, per_u);
  eng_flow_resume(eng, per_u);

  //  hear what they sent before we knew them
  //
  if ( per_u->stl_u ) {
    eng_out*    stl_u = per_u->stl_u;
    const c3_y* hea_y = eng->hea_y;
    c3_z        hea_z = eng->hea_z;

    per_u->stl_u = 0;
    per_u->stn_w = 0;
    while ( stl_u ) {
      eng_out* nex_u = stl_u->nex_u;
      eng_hear_pact(eng, stl_u->lan_u, stl_u->buf_y, stl_u->len_z);
      c3_free(stl_u->buf_y);
      c3_free(stl_u);
      stl_u = nex_u;
    }
    eng->hea_y = hea_y;
    eng->hea_z = hea_z;
  }
}

/* comets
*/
static c3_c*
_proof_path(mesa_engine* eng)
{
  c3_c  lyf_c[32];
  c3_c* who_c = u3_ship_to_string(eng->our_u);
  c3_c* out_c = c3_malloc(strlen(who_c) + 64);

  knot_ud(eng->lyf_d, lyf_c);
  sprintf(out_c, "publ/1/a/x/1//pawn/proof/%s/%s", who_c, lyf_c);
  c3_free(who_c);
  return out_c;
}

/* eng_comet_proof(): +al-read-proof, peek a comet's attestation
*/
void
eng_comet_proof(mesa_engine* eng, eng_peer* per_u, mesa_lane lan_u)
{
  c3_c*    pat_c = _proof_path(eng);
  eng_req* req_u = eng_req_find(per_u, pat_c);

  if ( MESA_LANE_NONE != lan_u.kind ) {
    eng_peer_heard(eng, per_u, lan_u, 0, 0);
  }
  if ( !req_u ) {
    req_u = eng_req_add(eng, per_u, pat_c, REQ_PROOF);
    eng_req_send(eng, per_u, req_u);
  }
  c3_free(pat_c);
}

/* eng_comet_take(): +al-take-proof and +al-register-comet
**
**   [gag] is the proof page, unauthenticated: its signature (over
**   [roo_y], bound at [pat_c]) is checked against the key it carries.
**   On success the comet has keys.
*/
c3_o
eng_comet_take(mesa_engine* eng, eng_peer* per_u, const c3_c* pat_c,
               const c3_y roo_y[32], u3_auth_data* aut_u, mesa_noun gag)
{
  mesa_noun pro, opa, pas;
  c3_y      pas_y[65];
  u3_ship   snd_u, rcv_u;
  c3_d      sly_d, rly_d;
  c3_y      buf_y[16];
  mesa_ship who = { .lo = per_u->who_u[0], .hi = per_u->who_u[1] };
  c3_o      ret_o = c3n;

  //  [%message %proof @]
  //
  if (  !mesa_is_cell(gag)
     || !mesa_is_cell(mesa_tail(gag))
     || !mesa_is_atom(mesa_at(6, gag))
     || !mesa_is_atom(mesa_at(7, gag)) )
  {
    return c3n;
  }

  pro = mesa_cue(mesa_at(7, gag));
  if ( MESA_NONE == pro ) {
    return c3n;
  }
  if ( !mesa_is_cell(pro) || !mesa_is_atom(mesa_tail(pro)) ) {
    mesa_unref(pro);
    return c3n;
  }

  opa = mesa_cue(mesa_tail(pro));
  mesa_unref(pro);
  if ( MESA_NONE == opa ) {
    return c3n;
  }

  //  [pass sndr sndr-life rcvr rcvr-life]
  //
  pas = mesa_at(2, opa);
  if (  (MESA_NONE == mesa_at(30, opa))
     || !mesa_is_atom(pas) || (mesa_met3(pas) > 65)
     || !mesa_is_atom(mesa_at(6, opa)) || (mesa_met3(mesa_at(6, opa)) > 16)
     || !mesa_is_atom(mesa_at(30, opa)) || (mesa_met3(mesa_at(30, opa)) > 16)
     || !mesa_atom_get_u64(mesa_at(14, opa), &sly_d)
     || !mesa_atom_get_u64(mesa_at(31, opa), &rly_d) )
  {
    mesa_unref(opa);
    return c3n;
  }

  mesa_atom_get_bytes(pas, 65, pas_y);
  mesa_atom_get_bytes(mesa_at(6, opa), 16, buf_y);
  u3_ship_of_bytes(snd_u, 16, buf_y);
  mesa_atom_get_bytes(mesa_at(30, opa), 16, buf_y);
  u3_ship_of_bytes(rcv_u, 16, buf_y);
  mesa_unref(opa);

  //  the proof is from this comet, for us at our current life, and the
  //  key is the comet's
  //
  if (  (c3y == u3_ships_equal(snd_u, per_u->who_u))
     && (1 == sly_d)
     && (c3y == u3_ships_equal(rcv_u, eng->our_u))
     && (rly_d == eng->lyf_d)
     && mesa_comet_check(who, pas_y)
     && (AUTH_SIGN == aut_u->typ_e) )
  {
    c3_c* pax_c = c3_malloc(strlen(pat_c) + 2);
    c3_c* ful_c;

    pax_c[0] = '/';
    strcpy(pax_c + 1, pat_c);
    ful_c = knot_beam(per_u->who_u, pax_c);
    ret_o = mesa_verify_binding(pas_y + 1, aut_u->sig_y, ful_c, roo_y);
    c3_free(pax_c);
    c3_free(ful_c);
  }

  if ( c3y == ret_o ) {
    u3_ship_sein(per_u->spo_u, per_u->who_u);
    per_u->spo_o = c3y;
    eng_peer_set_keys(eng, per_u, 0, 1, pas_y);
  }
  return ret_o;
}

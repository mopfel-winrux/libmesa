/// @file
///
/// Requests we make: the .pit (+ma-push-pact, +ma-pact, +hear-page in
/// ames.hoon) and multi-fragment downloads (_mesa_req_pact_init/_done
/// and the congestion control in vere's mesa.c).

#include "eng.h"

eng_req*
eng_req_find(eng_peer* per_u, const c3_c* pat_c)
{
  eng_req_map_itr itr_u = eng_req_map_get(&per_u->rem_u, (c3_c*)pat_c);
  return eng_req_map_is_end(itr_u) ? 0 : itr_u.data->val;
}

static void
_req_index(eng_peer* per_u, eng_req* req_u)
{
  if ( eng_req_map_is_end(eng_req_map_insert(&per_u->rem_u, req_u->pat_c, req_u)) ) {
    mesa_fail(__FILE__, __LINE__, "out of memory");
  }
}

eng_req*
eng_req_add(mesa_engine* eng, eng_peer* per_u, const c3_c* pat_c,
            eng_req_e typ_e)
{
  eng_req* req_u = c3_calloc(sizeof(*req_u));

  req_u->pat_c = strdup(pat_c);
  req_u->typ_e = typ_e;
  req_u->nex_d = eng->now_d;
  req_u->rto_d = ENG_RTO_MIN;
  req_u->aut_o = c3n;
  req_u->shu_o = c3n;
  req_u->wai_o = c3n;
  req_u->fly_o = c3n;

  req_u->pre_u = per_u->ret_u;
  if ( per_u->ret_u ) per_u->ret_u->nex_u = req_u;
  else                per_u->req_u = req_u;
  per_u->ret_u = req_u;
  _req_index(per_u, req_u);
  return req_u;
}

/* eng_req_rename(): change [req_u]'s path (takes [pat_c])
*/
void
eng_req_rename(eng_peer* per_u, eng_req* req_u, c3_c* pat_c)
{
  eng_req_map_erase(&per_u->rem_u, req_u->pat_c);
  c3_free(req_u->pat_c);
  req_u->pat_c = pat_c;
  _req_index(per_u, req_u);
}

static void
_fetch_free(eng_fetch* fet_u)
{
  if ( fet_u ) {
    c3_free(fet_u->dat_y);
    c3_free(fet_u->got_y);
    c3_free(fet_u->sen_d);
    c3_free(fet_u->sno_d);
    c3_free(fet_u->rsn_d);
    c3_free(fet_u->par_u);
    c3_free(fet_u->pap_y);
    arena_free(&fet_u->are_u);
    c3_free(fet_u);
  }
}

/* _req_reset(): drop [req_u]'s download and its slots in the window
*/
static void
_req_reset(eng_peer* per_u, eng_req* req_u)
{
  if ( req_u->fet_u ) {
    per_u->fly_d -= c3_min(per_u->fly_d, req_u->fet_u->out_d);
    _fetch_free(req_u->fet_u);
    req_u->fet_u = 0;
  }
  if ( c3y == req_u->fly_o ) {
    req_u->fly_o = c3n;
    if ( per_u->fly_d ) per_u->fly_d--;
  }
}

void
eng_req_free(eng_req* req_u)
{
  c3_free(req_u->pat_c);
  c3_free(req_u->pay_c);
  c3_free(req_u->key_c);
  memset(req_u->shk_y, 0, sizeof(req_u->shk_y));
  _fetch_free(req_u->fet_u);
  c3_free(req_u);
}

void
eng_req_del(eng_peer* per_u, eng_req* req_u)
{
  _req_reset(per_u, req_u);
  eng_req_map_erase(&per_u->rem_u, req_u->pat_c);
  if ( req_u->pre_u ) req_u->pre_u->nex_u = req_u->nex_u;
  else                per_u->req_u = req_u->nex_u;
  if ( req_u->nex_u ) req_u->nex_u->pre_u = req_u->pre_u;
  else                per_u->ret_u = req_u->pre_u;
  req_u->nex_u = req_u->pre_u = 0;
}

/* eng_req_clear(): forget all of [per_u]'s requests
*/
void
eng_req_clear(eng_peer* per_u)
{
  while ( per_u->req_u ) {
    eng_req* nex_u = per_u->req_u->nex_u;
    eng_req_free(per_u->req_u);
    per_u->req_u = nex_u;
  }
  per_u->ret_u = 0;
  per_u->fly_d = 0;
  eng_req_map_clear(&per_u->rem_u);
}

/* _name(): a name in [per_u]'s namespace
*/
static void
_name(u3_mesa_name* nam_u, eng_peer* per_u, const c3_c* pat_c)
{
  memset(nam_u, 0, sizeof(*nam_u));
  u3_ship_copy(nam_u->her_u, per_u->who_u);
  nam_u->rif_h = (c3_h)per_u->rif_d;
  nam_u->boq_y = ENG_BOQ;
  nam_u->nit_o = c3y;
  nam_u->aut_o = c3n;
  nam_u->fra_d = 0;
  nam_u->pat_c = (c3_c*)pat_c;
  nam_u->pat_s = (c3_s)strlen(pat_c);
}

static void
_send_lanes(mesa_engine* eng, eng_peer* per_u, u3_mesa_pact* pac_u)
{
  mesa_lane las_u[2];
  c3_w      len_w = eng_peer_lanes(eng, per_u, las_u);

  for ( c3_w i = 0; i < len_w; i++ ) {
    eng_send_pact(eng, las_u[i], pac_u);
  }
}

/* _send_poke(): +ma-pact for a %poke; the first page of our payload
**               rides along with the request for their ack
*/
static void
_send_poke(mesa_engine* eng, eng_peer* per_u, eng_req* req_u)
{
  u3_mesa_pact pac_u;
  eng_line*    lin_u = eng_line_get(eng, req_u->pay_c);
  c3_y*        fre_y = 0;
  c3_y         roo_y[32];

  if ( !lin_u ) {
    mesa_log("mesa: poke: missing payload %s", req_u->pay_c);
    return;
  }

  memset(&pac_u, 0, sizeof(pac_u));
  pac_u.hed_u.typ_y = PACT_POKE;
  _name(&pac_u.pok_u.nam_u, per_u, req_u->pat_c);

  memset(&pac_u.pok_u.pay_u, 0, sizeof(u3_mesa_name));
  u3_ship_copy(pac_u.pok_u.pay_u.her_u, eng->our_u);
  pac_u.pok_u.pay_u.rif_h = (c3_h)eng->rif_d;
  pac_u.pok_u.pay_u.boq_y = ENG_BOQ;
  pac_u.pok_u.pay_u.nit_o = c3y;
  pac_u.pok_u.pay_u.pat_c = lin_u->pat_c;
  pac_u.pok_u.pay_u.pat_s = (c3_s)strlen(lin_u->pat_c);

  eng_page_first(eng, lin_u, &pac_u.pok_u.dat_u, &fre_y);

  //  over the MTU: send the root instead and let them +peek the rest
  //
  if ( (1 == lin_u->tof_d) && (mesa_size_pact(&pac_u) > ENG_MTU) ) {
    mesa_blake3(lin_u->tob_d, lin_u->ser_y, 32, roo_y);
    pac_u.pok_u.dat_u.fra_y = roo_y;
    pac_u.pok_u.dat_u.len_h = 32;
  }

  _send_lanes(eng, per_u, &pac_u);
  c3_free(fre_y);
}

static void
_send_peek(mesa_engine* eng, eng_peer* per_u, const c3_c* pat_c,
           c3_o nit_o, c3_d fag_d)
{
  u3_mesa_pact pac_u;

  memset(&pac_u, 0, sizeof(pac_u));
  pac_u.hed_u.typ_y = PACT_PEEK;
  _name(&pac_u.pek_u.nam_u, per_u, pat_c);
  if ( c3n == nit_o ) {
    pac_u.pek_u.nam_u.nit_o = c3n;
    pac_u.pek_u.nam_u.fra_d = fag_d;
  }
  _send_lanes(eng, per_u, &pac_u);
}

/* congestion control, after vere's _mesa_handle_ack (times in ms)
*/
static c3_d
_clamp_rto(c3_d rto_d)
{
  return c3_min(c3_max(rto_d, 200), 25000);
}

static void
_gauge_ack(eng_peer* per_u, c3_d sen_d, c3_d now_d)
{
  c3_d rtt_d = ( now_d < sen_d ) ? 0 : now_d - sen_d;
  c3_d err_d = ( rtt_d > per_u->rtt_d ) ? rtt_d - per_u->rtt_d
                                        : per_u->rtt_d - rtt_d;

  per_u->rto_d = _clamp_rto(per_u->rtt_d + (4 * per_u->rtv_d));

  if ( per_u->wnd_d < per_u->sst_d ) {
    per_u->wnd_d++;
  }
  else if ( per_u->wnd_d <= ++per_u->wnf_d ) {
    per_u->wnd_d++;
    per_u->wnf_d = 0;
  }

  per_u->rtt_d = (rtt_d + (per_u->rtt_d * 7)) >> 3;
  per_u->rtv_d = (err_d + (per_u->rtv_d * 7)) >> 3;
}

/* _gauge_loss(): a request we sent as [sno_d] was lost. Cut the window
**                once per round: not again for losses of requests sent
**                before the cut (NewReno's recovery point).
*/
static void
_gauge_loss(eng_peer* per_u, c3_d sno_d, c3_o tim_o)
{
  if ( sno_d <= per_u->rec_d ) {
    return;
  }
  per_u->rec_d = per_u->snd_d;
  per_u->sst_d = c3_max(1, per_u->wnd_d / 2);
  per_u->wnd_d = per_u->sst_d;
  if ( c3y == tim_o ) {
    per_u->rto_d = _clamp_rto(per_u->rto_d * 2);
  }
}

/* _send_frag(): request fragment [fag_d] of [req_u]'s download
*/
static void
_send_frag(mesa_engine* eng, eng_peer* per_u, eng_req* req_u, c3_d fag_d)
{
  eng_fetch* fet_u = req_u->fet_u;

  fet_u->sen_d[fag_d] = eng->now_d;
  fet_u->sno_d[fag_d] = ++per_u->snd_d;
  _send_peek(eng, per_u, req_u->pat_c, c3n, fag_d);
}

static void _req_first(mesa_engine* eng, eng_peer* per_u, eng_req* req_u);
static void _first_fast(mesa_engine* eng, eng_peer* per_u, c3_d sno_d);
static void _fetch_again(mesa_engine* eng, eng_peer* per_u, eng_req* req_u,
                         c3_d fag_d);

/* _fetch_pump(): request fragments, and start scries, while the window
**                is open
**
**   The window is the peer's, shared by all our requests to them (one
**   path between us), round-robin, a fragment each per round.
*/
static void
_fetch_pump(mesa_engine* eng, eng_peer* per_u)
{
  c3_o pro_o = c3y;

  while ( (c3y == pro_o) && (per_u->fly_d < per_u->wnd_d) ) {
    pro_o = c3n;

    for ( eng_req* req_u = per_u->req_u;
          req_u && (per_u->fly_d < per_u->wnd_d);
          req_u = req_u->nex_u )
    {
      eng_fetch* fet_u = req_u->fet_u;

      if ( c3y == req_u->wai_o ) {
        _req_first(eng, per_u, req_u);
        pro_o = c3y;
        continue;
      }
      if ( !fet_u ) continue;
      while ( (fet_u->nex_d < fet_u->tof_d) && fet_u->got_y[fet_u->nex_d] ) {
        fet_u->nex_d++;
      }
      if ( fet_u->nex_d >= fet_u->tof_d ) continue;

      fet_u->out_d++;
      per_u->fly_d++;
      pro_o = c3y;
      _send_frag(eng, per_u, req_u, fet_u->nex_d++);
    }
  }

  for ( eng_req* req_u = per_u->req_u; req_u; req_u = req_u->nex_u ) {
    if ( req_u->fet_u ) {
      req_u->nex_d = c3_min(req_u->nex_d, eng->now_d + per_u->rto_d);
    }
  }
}

/* _fetch_resend(): re-request fragments that timed out
*/
static void
_fetch_resend(mesa_engine* eng, eng_peer* per_u, eng_req* req_u)
{
  eng_fetch* fet_u = req_u->fet_u;

  for ( c3_d i = fet_u->hav_d; i < fet_u->nex_d; i++ ) {
    if (  !fet_u->got_y[i]
       && (eng->now_d >= fet_u->sen_d[i] + per_u->rto_d) )
    {
      _gauge_loss(per_u, fet_u->sno_d[i], c3y);
      _fetch_again(eng, per_u, req_u, i);
    }
  }
  req_u->nex_d = eng->now_d + per_u->rto_d;
  _fetch_pump(eng, per_u);
}

/* _overtaken(): is a request we sent as [old_d] lost, now that the one
**               we sent as [sno_d] has come back? Yes once three sent
**               after it have (the path reorders little), or one has
**               and fewer than three were (its tail, which would
**               otherwise wait for the timer).
*/
static c3_o
_overtaken(eng_peer* per_u, c3_d old_d, c3_d sno_d)
{
  return __((old_d < sno_d) && (  (old_d + 3 <= sno_d)
                               || (per_u->snd_d < old_d + 3)));
}

/* _fetch_again(): request fragment [fag_d] again, and watch that too
*/
static void
_fetch_again(mesa_engine* eng, eng_peer* per_u, eng_req* req_u, c3_d fag_d)
{
  eng_fetch* fet_u = req_u->fet_u;
  c3_w       i_w;

  _send_frag(eng, per_u, req_u, fag_d);

  for ( i_w = 0; i_w < fet_u->rsn_w; i_w++ ) {
    if ( fet_u->rsn_d[i_w] == fag_d ) return;
  }
  if ( fet_u->rsn_w == fet_u->rsc_w ) {
    fet_u->rsc_w = c3_max(8, 2 * fet_u->rsc_w);
    fet_u->rsn_d = c3_realloc(fet_u->rsn_d, fet_u->rsc_w * sizeof(c3_d));
  }
  fet_u->rsn_d[fet_u->rsn_w++] = fag_d;
}

/* _fetch_fast(): ask again now for fragments that later requests have
**                overtaken, instead of waiting for the timer: those we
**                asked for again, then new ones in order
*/
static void
_fetch_fast(mesa_engine* eng, eng_peer* per_u, eng_req* req_u, c3_d sno_d)
{
  eng_fetch* fet_u = req_u->fet_u;
  c3_d       i;

  for ( c3_w i_w = 0; i_w < fet_u->rsn_w; ) {
    c3_d fag_d = fet_u->rsn_d[i_w];

    if ( fet_u->got_y[fag_d] ) {
      fet_u->rsn_d[i_w] = fet_u->rsn_d[--fet_u->rsn_w];
      continue;
    }
    if ( c3y == _overtaken(per_u, fet_u->sno_d[fag_d], sno_d) ) {
      _gauge_loss(per_u, fet_u->sno_d[fag_d], c3n);
      _send_frag(eng, per_u, req_u, fag_d);
    }
    i_w++;
  }

  for ( i = c3_max(fet_u->fas_d, fet_u->hav_d); i < fet_u->nex_d; i++ ) {
    if ( fet_u->got_y[i] ) continue;
    if ( c3n == _overtaken(per_u, fet_u->sno_d[i], sno_d) ) break;
    _gauge_loss(per_u, fet_u->sno_d[i], c3n);
    _fetch_again(eng, per_u, req_u, i);
  }
  fet_u->fas_d = i;
}

/* _first_fast(): the same for scries' first requests, in the order we
**                made them (resends, sent later, don't stop the scan)
*/
static void
_first_fast(mesa_engine* eng, eng_peer* per_u, c3_d sno_d)
{
  for ( eng_req* req_u = per_u->req_u; req_u; req_u = req_u->nex_u ) {
    if ( c3n == req_u->fly_o ) continue;
    if ( c3y == _overtaken(per_u, req_u->sno_d, sno_d) ) {
      _gauge_loss(per_u, req_u->sno_d, c3n);
      req_u->sen_d = eng->now_d;
      req_u->sno_d = ++per_u->snd_d;
      req_u->try_w++;
      req_u->nex_d = eng->now_d + req_u->rto_d;
      _send_peek(eng, per_u, req_u->pat_c, c3y, 0);
    }
    else if ( 1 == req_u->try_w ) {
      break;
    }
  }
}

/* _req_first(): send a request's first page request
*/
static void
_req_first(mesa_engine* eng, eng_peer* per_u, eng_req* req_u)
{
  //  a scry waits for a slot in the window; once it has one it keeps
  //  it until the first page comes back
  //
  if ( REQ_KEEN == req_u->typ_e ) {
    if ( c3n == req_u->fly_o ) {
      if ( per_u->fly_d >= per_u->wnd_d ) {
        req_u->wai_o = c3y;
        req_u->nex_d = UINT64_MAX;
        return;
      }
      req_u->fly_o = c3y;
      per_u->fly_d++;
    }
    else {
      _gauge_loss(per_u, req_u->sno_d, c3y);
    }
    if ( !req_u->try_w ) {
      req_u->rto_d = per_u->rto_d;   //  we know this path: time it
    }
    req_u->wai_o = c3n;
    req_u->sen_d = eng->now_d;
    req_u->sno_d = ++per_u->snd_d;
  }

  if ( REQ_ACK == req_u->typ_e ) {
    _send_poke(eng, per_u, req_u);
  }
  else {
    _send_peek(eng, per_u, req_u->pat_c, c3y, 0);
  }

  req_u->nex_d = eng->now_d + req_u->rto_d;
  req_u->rto_d = c3_min(req_u->rto_d * 2, ENG_RTO_MAX);
  req_u->try_w++;
}

/* _req_landed(): the first page of [req_u] came back
*/
static void
_req_landed(mesa_engine* eng, eng_peer* per_u, eng_req* req_u)
{
  if ( c3y == req_u->fly_o ) {
    req_u->fly_o = c3n;
    if ( per_u->fly_d ) per_u->fly_d--;
    if ( 1 == req_u->try_w ) {   //  (Karn: no samples from resends)
      _gauge_ack(per_u, req_u->sen_d, eng->now_d);
    }
    _first_fast(eng, per_u, req_u->sno_d);
  }
}

/* eng_req_send(): (re)send a request
*/
void
eng_req_send(mesa_engine* eng, eng_peer* per_u, eng_req* req_u)
{
  if ( (c3n == per_u->kno_o) && (REQ_PROOF != req_u->typ_e) ) {
    eng_peer_ask(eng, per_u);
    req_u->nex_d = eng->now_d + ENG_RTO_MIN;
    return;
  }

  if ( req_u->fet_u ) {
    _fetch_resend(eng, per_u, req_u);
    return;
  }
  _req_first(eng, per_u, req_u);
}

void
eng_req_wake(mesa_engine* eng, eng_peer* per_u)
{
  eng_req* req_u = per_u->req_u;

  while ( req_u ) {
    eng_req* nex_u = req_u->nex_u;
    if ( req_u->nex_d <= eng->now_d ) {
      //  a comet that won't attest (or never existed): give up
      //
      if ( (REQ_PROOF == req_u->typ_e) && (req_u->try_w >= 6) ) {
        eng_req_del(per_u, req_u);
        eng_req_free(req_u);
      }
      else {
        eng_req_send(eng, per_u, req_u);
      }
    }
    req_u = nex_u;
  }
}

c3_d
eng_req_deadline(eng_peer* per_u)
{
  c3_d dea_d = UINT64_MAX;

  for ( eng_req* req_u = per_u->req_u; req_u; req_u = req_u->nex_u ) {
    dea_d = c3_min(dea_d, req_u->nex_d);
  }
  return dea_d;
}

/* _req_done(): a complete, authenticated message for [req_u]
*/
static void
_req_done(mesa_engine* eng, eng_peer* per_u, eng_req* req_u,
          c3_z len_z, const c3_y* ser_y)
{
  c3_z      pla_z;
  c3_y*     pla_y = ( c3y == req_u->shu_o )
                    ? eng_open_shut(req_u->shk_y, req_u->pat_c, len_z, ser_y, &pla_z)
                    : eng_open_page(eng, per_u, req_u->pat_c, len_z, ser_y, &pla_z);
  mesa_noun gag;

  eng_req_del(per_u, req_u);

  if ( !pla_y ) {
    mesa_log("mesa: %s: undecryptable page", eng_ship_text(per_u->who_u));
    eng_req_free(req_u);
    return;
  }

  gag = mesa_cue_bytes(pla_z, pla_y);
  c3_free(pla_y);

  if ( MESA_NONE == gag ) {
    mesa_log("mesa: %s: malformed page", eng_ship_text(per_u->who_u));
    eng_req_free(req_u);
    return;
  }

  switch ( req_u->typ_e ) {
    case REQ_KEEN: {
      if ( eng->hok_u.sage ) {
        mesa_ship her = { .lo = per_u->who_u[0], .hi = per_u->who_u[1] };
        eng->hok_u.sage(eng->hok_u.ctx, her, req_u->key_c, gag);
      }
    } break;

    case REQ_ACK:
    case REQ_NAXP:
    case REQ_CORK:
    case REQ_POKE: {
      eng_flow_take(eng, per_u, req_u, gag);
    } break;

    default: break;
  }

  mesa_unref(gag);
  eng_req_free(req_u);
}

/* _fetch_init(): the proof page of a multi-fragment message
*/
static void
_fetch_init(mesa_engine* eng, eng_peer* per_u, eng_req* req_u,
            u3_mesa_data* dat_u, lss_hash* pof_u, c3_d pon_d)
{
  eng_fetch* fet_u = c3_calloc(sizeof(*fet_u));
  c3_d       tof_d = mesa_num_leaves(dat_u->tob_d);

  fet_u->tob_d = dat_u->tob_d;
  fet_u->tof_d = tof_d;
  fet_u->dat_y = c3_calloc(fet_u->tob_d);
  fet_u->got_y = c3_calloc(tof_d);
  fet_u->sen_d = c3_calloc(tof_d * sizeof(c3_d));
  fet_u->sno_d = c3_calloc(tof_d * sizeof(c3_d));
  fet_u->par_u = c3_calloc(tof_d * sizeof(lss_pair));
  fet_u->pap_y = c3_calloc(tof_d);
  fet_u->are_u = arena_create(64 * (c3_bits_half((c3_h)tof_d) + 2));
  lss_verifier_init(&fet_u->los_u, 0, (c3_h)tof_d, pof_u, &fet_u->are_u);

  req_u->fet_u = fet_u;
  req_u->nex_d = eng->now_d + per_u->rto_d;
  _fetch_pump(eng, per_u);
}

/* _fetch_take(): a %data fragment
*/
static void
_fetch_take(mesa_engine* eng, eng_peer* per_u, eng_req* req_u,
            u3_mesa_name* nam_u, u3_mesa_data* dat_u)
{
  eng_fetch* fet_u = req_u->fet_u;
  c3_d       fag_d = nam_u->fra_d;
  c3_z       len_z;

  if (  (dat_u->tob_d != fet_u->tob_d)
     || (fag_d >= fet_u->tof_d)
     || fet_u->got_y[fag_d] )
  {
    return;
  }

  len_z = c3_min(fet_u->tob_d - (fag_d * ENG_FRAG), ENG_FRAG);
  if ( dat_u->len_h != len_z ) {
    return;
  }

  memcpy(fet_u->dat_y + (fag_d * ENG_FRAG), dat_u->fra_y, len_z);
  if ( AUTH_PAIR == dat_u->aut_u.typ_e ) {
    memcpy(fet_u->par_u[fag_d], dat_u->aut_u.has_y, sizeof(lss_pair));
    fet_u->pap_y[fag_d] = 1;
  }
  fet_u->got_y[fag_d] = 1;
  if ( fet_u->out_d ) {
    fet_u->out_d--;
    if ( per_u->fly_d ) per_u->fly_d--;
  }
  _gauge_ack(per_u, fet_u->sen_d[fag_d], eng->now_d);

  //  every download from them shares the path this came over
  //
  for ( eng_req* fas_u = per_u->req_u; fas_u; fas_u = fas_u->nex_u ) {
    if ( fas_u->fet_u ) _fetch_fast(eng, per_u, fas_u, fet_u->sno_d[fag_d]);
  }

  //  verify everything we now have in order
  //
  while ( (fet_u->hav_d < fet_u->tof_d) && fet_u->got_y[fet_u->hav_d] ) {
    c3_d i = fet_u->hav_d;
    c3_z lez_z = c3_min(fet_u->tob_d - (i * ENG_FRAG), ENG_FRAG);

    if ( c3y != lss_verifier_ingest(&fet_u->los_u,
                                    fet_u->dat_y + (i * ENG_FRAG),
                                    (c3_h)lez_z,
                                    fet_u->pap_y[i] ? &fet_u->par_u[i] : 0) )
    {
      //  forged or corrupt; start over
      //
      mesa_log("mesa: %s: fragment %" PRIu64 " failed verification",
               eng_ship_text(per_u->who_u), i);
      _req_reset(per_u, req_u);
      req_u->aut_o = c3n;
      req_u->nex_d = eng->now_d + per_u->rto_d;
      return;
    }
    fet_u->hav_d++;
  }

  if ( fet_u->hav_d == fet_u->tof_d ) {
    c3_y* dat_y = fet_u->dat_y;
    c3_z  tob_z = fet_u->tob_d;

    fet_u->dat_y = 0;
    _req_done(eng, per_u, req_u, tob_z, dat_y);
    c3_free(dat_y);
  }

  //  its window slots go to whoever needs them
  //
  _fetch_pump(eng, per_u);
}

/* eng_hear_page(): +hear-page:ev-pact
*/
void
eng_hear_page(mesa_engine* eng, mesa_lane lan_u, u3_mesa_pact* pac_u)
{
  u3_mesa_name* nam_u = &pac_u->pag_u.nam_u;
  u3_mesa_data* dat_u = &pac_u->pag_u.dat_u;
  eng_peer*     per_u = eng_peer_get(eng, nam_u->her_u);
  eng_req*      req_u;
  c3_c*         pat_c;
  mesa_lane     nex_u, *nep_u = 0;

  if ( !per_u || (ENG_BOQ != nam_u->boq_y) ) {
    return;
  }

  pat_c = c3_malloc(nam_u->pat_s + 1);
  memcpy(pat_c, nam_u->pat_c, nam_u->pat_s);
  pat_c[nam_u->pat_s] = 0;
  req_u = eng_req_find(per_u, pat_c);
  c3_free(pat_c);

  if ( !req_u ) {
    return;
  }
  if ( (c3n == per_u->kno_o) && (REQ_PROOF != req_u->typ_e) ) {
    return;
  }
  if ( nam_u->rif_h != (c3_h)per_u->rif_d ) {
    return;
  }

  if ( HOP_SHORT == pac_u->hed_u.nex_y ) {
    nex_u = (mesa_lane){ .kind = MESA_LANE_IP,
                         .ip   = c3_sift_half(pac_u->pag_u.sot_u),
                         .port = c3_sift_short(pac_u->pag_u.sot_u + 4) };
    nep_u = &nex_u;
  }
  eng_peer_heard(eng, per_u, lan_u, pac_u->hed_u.hop_y, nep_u);

  //  first page: the whole message, or the proof of a longer one
  //
  if ( c3y == nam_u->nit_o ) {
    c3_d tof_d = mesa_num_leaves(dat_u->tob_d);
    c3_y roo_y[32];

    if ( c3y == req_u->aut_o ) {
      return;   //  duplicate
    }
    if (  (AUTH_SIGN != dat_u->aut_u.typ_e)
       && (AUTH_HMAC != dat_u->aut_u.typ_e) )
    {
      return;
    }

    if ( 1 == tof_d ) {
      if ( dat_u->len_h != dat_u->tob_d ) {
        return;
      }
      mesa_blake3(dat_u->len_h, dat_u->fra_y, 32, roo_y);

      if ( REQ_PROOF == req_u->typ_e ) {
        mesa_noun gag = mesa_cue_bytes(dat_u->len_h, dat_u->fra_y);

        if ( MESA_NONE == gag ) {
          return;
        }
        if ( c3y == eng_comet_take(eng, per_u, req_u->pat_c, roo_y,
                                   &dat_u->aut_u, gag) )
        {
          eng_req_del(per_u, req_u);
          eng_req_free(req_u);
          eng_peer_known(eng, per_u);
        }
        mesa_unref(gag);
        return;
      }

      if ( c3n == eng_auth_page(eng, per_u, req_u->pat_c, roo_y, &dat_u->aut_u) ) {
        mesa_log("mesa: %s: page failed authentication",
                 eng_ship_text(per_u->who_u));
        return;
      }
      _req_landed(eng, per_u, req_u);
      _req_done(eng, per_u, req_u, dat_u->len_h, dat_u->fra_y);
      _fetch_pump(eng, per_u);
      return;
    }

    //  multi-fragment: authenticate the proof's root, then fetch
    //
    {
      c3_d      pon_d = lss_proof_size(tof_d);
      lss_hash* pof_u;

      if (  (REQ_PROOF == req_u->typ_e)
         || (dat_u->tob_d > ENG_MAX_MSG)
         || (dat_u->len_h != pon_d * sizeof(lss_hash)) )
      {
        return;
      }
      pof_u = c3_malloc(pon_d * sizeof(lss_hash));
      memcpy(pof_u, dat_u->fra_y, pon_d * sizeof(lss_hash));
      lss_recover_root(roo_y, pof_u, (c3_h)pon_d);

      if ( c3n == eng_auth_page(eng, per_u, req_u->pat_c, roo_y, &dat_u->aut_u) ) {
        mesa_log("mesa: %s: proof failed authentication",
                 eng_ship_text(per_u->who_u));
        c3_free(pof_u);
        return;
      }
      req_u->aut_o = c3y;
      _req_landed(eng, per_u, req_u);
      _fetch_init(eng, per_u, req_u, dat_u, pof_u, pon_d);
      c3_free(pof_u);
      return;
    }
  }

  //  a %data fragment of a download in progress
  //
  if ( (c3n == nam_u->aut_o) && req_u->fet_u ) {
    _fetch_take(eng, per_u, req_u, nam_u, dat_u);
  }
}

/* eng_keen_path(): the namespace path for a remote scry: public, or
**                  %shut with the request's key
*/
c3_c*
eng_keen_path(eng_peer* per_u, eng_req* req_u)
{
  if ( c3y == req_u->shu_o ) {
    return eng_shut_path(req_u->kid_d, req_u->shk_y, req_u->key_c);
  }
  return eng_publ_path(per_u->lyf_d, req_u->key_c);
}

/* eng_req_rebase(): [per_u]'s keys changed; paths that name their life
**                   are stale. Rename scries; flows restart their own.
*/
void
eng_req_rebase(mesa_engine* eng, eng_peer* per_u)
{
  eng_req* req_u = per_u->req_u;

  while ( req_u ) {
    eng_req* nex_u = req_u->nex_u;

    switch ( req_u->typ_e ) {
      case REQ_KEEN: {
        _req_reset(per_u, req_u);
        eng_req_rename(per_u, req_u, eng_keen_path(per_u, req_u));
        req_u->wai_o = c3n;
        req_u->aut_o = c3n;
        req_u->nex_d = eng->now_d;
        req_u->rto_d = ENG_RTO_MIN;
      } break;

      case REQ_PROOF: break;

      default: {
        eng_req_del(per_u, req_u);
        eng_req_free(req_u);
      } break;
    }
    req_u = nex_u;
  }
}

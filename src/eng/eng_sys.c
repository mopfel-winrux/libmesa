/// @file
///
/// System flows: the messages Arvo's vanes exchange with a peer on their
/// own behalf, which the engine speaks so applications don't have to.
///
///   gall /gf   flub tracking (+mo-track-flubs, +mo-handle-sys-flu): a
///              peer whose agent isn't running drops our pleas to it and
///              tells us to stop (%flub), then to go on (%spur); and the
///              same from us (+mo-do-flub, +mo-halt, +mo-give-halts), for
///              requests the application answers with MESA_FLUB
///   gall /gk   %shut scry key requests (+mo-handle-key-request): the
///              application's hooks.shut_key decides, and we answer
///              [%0 (unit brood)]
///   jael /public-keys   moon keys from the moon's parent (+sources:feel,
///              +public-keys:feed): there's no azimuth for moons

#include "eng.h"

static mesa_ship
_her(eng_peer* per_u)
{
  return (mesa_ship){ .lo = per_u->who_u[0], .hi = per_u->who_u[1] };
}

static c3_o
_cord_is(mesa_noun som, const c3_c* str_c)
{
  c3_c* som_c;
  c3_o  ret_o;

  if ( (MESA_NONE == som) || !mesa_is_atom(som) || (mesa_met3(som) > 64) ) {
    return c3n;
  }
  som_c = mesa_atom_get_cord(som);
  ret_o = __(0 == strcmp(som_c, str_c));
  c3_free(som_c);
  return ret_o;
}

static mesa_noun
_ship_noun(u3_ship who_u)
{
  c3_y buf_y[16];
  u3_ship_to_bytes(who_u, 16, buf_y);
  return mesa_atom_bytes(16, buf_y);
}

static c3_o
_noun_ship(mesa_noun som, u3_ship who_u)
{
  c3_y buf_y[16];

  if ( (MESA_NONE == som) || !mesa_is_atom(som) || (mesa_met3(som) > 16) ) {
    return c3n;
  }
  mesa_atom_get_bytes(som, 16, buf_y);
  u3_ship_of_bytes(who_u, 16, buf_y);
  return c3y;
}

/* _open(): a new forward flow to [per_u] for our own use
*/
static c3_d
_open(mesa_engine* eng, eng_peer* per_u)
{
  c3_d      bon_d = per_u->bon_d;
  eng_flow* flo_u;

  per_u->bon_d += 4;
  flo_u = eng_flow_make(eng, per_u, bon_d, DIRE_FOR);
  eng_flow_save(eng, per_u, flo_u);
  return bon_d;
}

static void
_plea(mesa_engine* eng, eng_peer* per_u, c3_d bon_d,
      const c3_c* van_c, mesa_noun pax, mesa_noun pay)
{
  eng_flow* flo_u = eng_flow_get(per_u, bon_d, DIRE_FOR);

  if ( !flo_u ) {
    mesa_unref(pax);
    mesa_unref(pay);
    return;
  }
  eng_flow_pump(eng, per_u, flo_u,
                mesa_qual(mesa_atom_cord("plea"), mesa_atom_cord(van_c),
                          pax, pay));
  if ( c3n == per_u->kno_o ) {
    eng_peer_ask(eng, per_u);
  }
}

c3_o
eng_sys_bone(eng_peer* per_u, c3_d bon_d)
{
  return __(  ((c3y == per_u->gfo_o) && (bon_d == per_u->gfb_d))
           || ((c3y == per_u->jao_o) && (bon_d == per_u->jab_d)) );
}

/*  gall
*/

/* eng_sys_track(): +mo-track-flubs; before our first gall request to
**                  [per_u], ask to hear about their agents' flubs
*/
void
eng_sys_track(mesa_engine* eng, eng_peer* per_u, mesa_noun msg)
{
  mesa_noun pax = mesa_at(14, msg);

  //  [%plea %g /ge/agent *]
  //
  if (  (c3y == per_u->gfo_o)
     || (c3n == _cord_is(mesa_at(6, msg), "g"))
     || (MESA_NONE == pax) || !mesa_is_cell(pax)
     || (c3n == _cord_is(mesa_head(pax), "ge")) )
  {
    return;
  }

  per_u->gfb_d = _open(eng, per_u);
  per_u->gfo_o = c3y;
  eng_peer_save(eng, per_u);

  //  [%g /gf %0 ~]
  //
  _plea(eng, per_u, per_u->gfb_d, "g",
        mesa_cell(mesa_atom_cord("gf"), mesa_atom_u64(0)),
        mesa_cell(mesa_atom_u64(0), mesa_atom_u64(0)));
}

/* _flub(): +mo-handle-sys-flu; [%0 %flub agent bone] or [%0 %spur agent]
*/
static void
_flub(mesa_engine* eng, eng_peer* per_u, mesa_noun pay)
{
  mesa_noun res = mesa_tail(pay);
  c3_d      bon_d;

  if ( !mesa_is_cell(res) ) {
    return;
  }

  if ( c3y == _cord_is(mesa_head(res), "flub") ) {
    eng_flow* flo_u;

    //  the bone is the one we sent the plea on
    //
    if (  !mesa_is_cell(mesa_tail(res))
       || !mesa_atom_get_u64(mesa_tail(mesa_tail(res)), &bon_d)
       || !(flo_u = eng_flow_get(per_u, bon_d, DIRE_FOR))
       || (c3y == eng_sys_bone(per_u, bon_d)) )
    {
      return;
    }
    eng_flow_halt(eng, per_u, flo_u);
    return;
  }

  if ( c3y == _cord_is(mesa_head(res), "spur") ) {
    //  resume every halted flow to them: if one's agent is still out,
    //  they'll flub it again
    //
    for ( eng_flow_map_itr itr_u = eng_flow_map_first(&per_u->flo_u);
          !eng_flow_map_is_end(itr_u);
          itr_u = eng_flow_map_next(itr_u) )
    {
      eng_flow* flo_u = itr_u.data->val;
      if ( (DIRE_FOR == flo_u->dir_e) && (c3y == flo_u->hat_o) ) {
        eng_flow_unhalt(eng, per_u, flo_u);
      }
    }
  }
}

/*  jael
*/

/* _apply(): a moon's point from its parent
*/
static void
_apply(mesa_engine* eng, eng_peer* par_u, u3_ship who_u, mesa_noun pon)
{
  //  [rift life keys=(map life [suite pass]) sponsor=(unit @p)]
  //
  mesa_point pot_u = { .who = { .lo = who_u[0], .hi = who_u[1] } };
  mesa_noun  sac[64];
  c3_w       len_w = 0;

  if (  !mesa_atom_get_u64(mesa_at(2, pon), &pot_u.rift)
     || !mesa_atom_get_u64(mesa_at(6, pon), &pot_u.life)
     || (MESA_NONE == mesa_at(15, pon)) )
  {
    return;
  }

  //  the pass for the current life (+on-publ-full skips points without);
  //  the map is a treap of [[life suite pass] l r]
  //
  if ( mesa_is_cell(mesa_at(14, pon)) ) sac[len_w++] = mesa_at(14, pon);
  while ( len_w ) {
    mesa_noun nod = sac[--len_w];
    mesa_noun kev = mesa_head(nod);
    mesa_noun pas = mesa_is_cell(kev) ? mesa_at(7, kev) : MESA_NONE;
    c3_d      lif_d;

    if (  mesa_is_cell(kev)
       && mesa_atom_get_u64(mesa_head(kev), &lif_d)
       && (lif_d == pot_u.life)
       && (MESA_NONE != pas) && mesa_is_atom(pas) && (mesa_met3(pas) <= 65) )
    {
      mesa_atom_get_bytes(pas, 65, pot_u.pass);
      break;
    }
    if ( mesa_is_cell(mesa_tail(nod)) ) {
      if ( mesa_is_cell(mesa_at(6, nod)) && (len_w < 64) ) sac[len_w++] = mesa_at(6, nod);
      if ( mesa_is_cell(mesa_at(7, nod)) && (len_w < 64) ) sac[len_w++] = mesa_at(7, nod);
    }
  }
  if ( 'b' != pot_u.pass[0] ) {
    return;
  }

  if ( mesa_is_cell(mesa_at(15, pon)) ) {
    u3_ship spo_u;
    if ( c3y == _noun_ship(mesa_tail(mesa_at(15, pon)), spo_u) ) {
      pot_u.has_sponsor = true;
      pot_u.sponsor = (mesa_ship){ .lo = spo_u[0], .hi = spo_u[1] };
    }
  }
  else {
    pot_u.has_sponsor = true;
    pot_u.sponsor = (mesa_ship){ .lo = par_u->who_u[0], .hi = par_u->who_u[1] };
  }

  eng_point_apply(eng, &pot_u);
}

/* _is_child(): [who_u] is a moon of [par_u]; a parent speaks for no one else
*/
static c3_o
_is_child(eng_peer* par_u, u3_ship who_u)
{
  u3_ship sei_u;

  if ( c3__earl != u3_ship_rank(who_u) ) {
    return c3n;
  }
  u3_ship_sein(sei_u, who_u);
  return u3_ships_equal(sei_u, par_u->who_u);
}

static void
_full(mesa_engine* eng, eng_peer* par_u, mesa_noun map)
{
  //  (map ship point): walk the treap [[ship point] l r]
  //
  mesa_noun sac[128];
  c3_w      len_w = 0;

  if ( mesa_is_cell(map) ) sac[len_w++] = map;

  while ( len_w ) {
    mesa_noun nod = sac[--len_w];
    mesa_noun kev = mesa_head(nod);
    u3_ship   who_u;

    if ( mesa_is_cell(kev) && (c3y == _noun_ship(mesa_head(kev), who_u))
       && (c3y == _is_child(par_u, who_u)) )
    {
      _apply(eng, par_u, who_u, mesa_tail(kev));
    }
    if ( mesa_is_cell(mesa_tail(nod)) ) {
      if ( mesa_is_cell(mesa_at(6, nod)) && (len_w < 128) ) sac[len_w++] = mesa_at(6, nod);
      if ( mesa_is_cell(mesa_at(7, nod)) && (len_w < 128) ) sac[len_w++] = mesa_at(7, nod);
    }
  }
}

static void
_diff(mesa_engine* eng, eng_peer* par_u, mesa_noun who, mesa_noun dif)
{
  u3_ship    who_u;
  eng_peer*  per_u;
  mesa_point pot_u;
  c3_y       pas_y[65];

  if (  (c3n == _noun_ship(who, who_u))
     || (c3n == _is_child(par_u, who_u))
     || !(per_u = eng_peer_get(eng, who_u))
     || (c3n == per_u->kno_o)
     || !mesa_is_cell(dif) || !mesa_is_cell(mesa_tail(dif)) )
  {
    return;
  }

  //  their point as we know it, changed
  //
  memset(&pot_u, 0, sizeof(pot_u));
  pot_u.who  = _her(per_u);
  pot_u.rift = per_u->rif_d;
  pot_u.life = per_u->lyf_d;
  pas_y[0] = 'b';
  memcpy(pas_y + 1, per_u->sgn_y, 32);
  memcpy(pas_y + 33, per_u->cry_y, 32);
  memcpy(pot_u.pass, pas_y, 65);
  pot_u.has_sponsor = (c3y == per_u->spo_o);
  pot_u.sponsor = (mesa_ship){ .lo = per_u->spo_u[0], .hi = per_u->spo_u[1] };

  if ( c3y == _cord_is(mesa_head(dif), "rift") ) {
    //  [%rift from to]
    if ( !mesa_atom_get_u64(mesa_tail(mesa_tail(dif)), &pot_u.rift) ) return;
  }
  else if ( c3y == _cord_is(mesa_head(dif), "keys") ) {
    //  [%keys from=[life suite pass] to=[life suite pass]]
    mesa_noun tox = mesa_tail(mesa_tail(dif));
    mesa_noun pas = mesa_at(7, tox);

    if (  !mesa_atom_get_u64(mesa_at(2, tox), &pot_u.life)
       || (MESA_NONE == pas) || !mesa_is_atom(pas) || (mesa_met3(pas) > 65) )
    {
      return;
    }
    memset(pot_u.pass, 0, 65);
    mesa_atom_get_bytes(pas, 65, pot_u.pass);
  }
  else if ( c3y == _cord_is(mesa_head(dif), "spon") ) {
    //  [%spon from to=(unit @p)]
    mesa_noun tox = mesa_tail(mesa_tail(dif));
    u3_ship   spo_u;

    if ( mesa_is_cell(tox) && (c3y == _noun_ship(mesa_tail(tox), spo_u)) ) {
      pot_u.sponsor = (mesa_ship){ .lo = spo_u[0], .hi = spo_u[1] };
    }
    else {
      pot_u.sponsor = _her(par_u);
    }
    pot_u.has_sponsor = true;
  }
  else {
    return;
  }

  eng_point_apply(eng, &pot_u);
}

/* _keys(): +public-keys:feel; [%public-keys-result public-keys-result]
*/
static void
_keys(mesa_engine* eng, eng_peer* par_u, mesa_noun pay)
{
  mesa_noun res = mesa_tail(pay);

  if (  (c3n == _cord_is(mesa_head(pay), "public-keys-result"))
     || !mesa_is_cell(res) )
  {
    return;
  }
  if ( c3y == _cord_is(mesa_head(res), "full") ) {
    _full(eng, par_u, mesa_tail(res));
  }
  else if ( c3y == _cord_is(mesa_head(res), "diff") && mesa_is_cell(mesa_tail(res)) ) {
    _diff(eng, par_u, mesa_at(6, res), mesa_at(7, res));
  }
  //  %breach: we work out breaches from rifts ourselves
}

/* eng_sys_moon(): ask a moon's parent for its keys (+sources:feel)
**
**   The parent keeps us up to date on that flow for good: one plea per
**   moon, and the flow is never closed.
*/
c3_o
eng_sys_moon(mesa_engine* eng, eng_peer* per_u)
{
  u3_ship   par_u;
  eng_peer* pap_u;

  if ( (c3__earl != u3_ship_rank(per_u->who_u)) || (c3y == eng->fak_o) ) {
    return c3n;
  }
  u3_ship_sein(par_u, per_u->who_u);
  if ( c3y == u3_ships_equal(par_u, eng->our_u) ) {
    return c3n;    //  our own moon: the application knows its keys
  }
  if ( c3y == per_u->moa_o ) {
    return c3y;
  }

  pap_u = eng_peer_make(eng, par_u);
  if ( c3n == pap_u->jao_o ) {
    pap_u->jab_d = _open(eng, pap_u);
    pap_u->jao_o = c3y;
    eng_peer_save(eng, pap_u);
  }

  per_u->moa_o = c3y;
  eng_peer_save(eng, per_u);

  //  [%j /public-keys %0 %public-keys whos=(set ship)]
  //
  _plea(eng, pap_u, pap_u->jab_d, "j",
        mesa_cell(mesa_atom_cord("public-keys"), mesa_atom_u64(0)),
        mesa_trel(mesa_atom_u64(0), mesa_atom_cord("public-keys"),
                  mesa_trel(_ship_noun(per_u->who_u),
                            mesa_atom_u64(0), mesa_atom_u64(0))));
  return c3y;
}

/* _unask(): we'll have to ask [par_u] about its moons again
*/
static void
_unask(mesa_engine* eng, eng_peer* par_u)
{
  for ( eng_peer_map_itr itr_u = eng_peer_map_first(&eng->per_u);
        !eng_peer_map_is_end(itr_u);
        itr_u = eng_peer_map_next(itr_u) )
  {
    eng_peer* per_u = itr_u.data->val;

    if ( (c3y == per_u->moa_o) && (c3y == _is_child(par_u, per_u->who_u)) ) {
      per_u->moa_o = c3n;
      eng_peer_save(eng, per_u);
    }
  }
}

/*  dispatch
*/

/* eng_sys_plea(): a system plea to us; c3y if we took it
*/
/* _brood(): +ap-serve-brood; [%0 ~] or [%0 ~ coop hutch=[rev idx key]]
*/
static mesa_noun
_brood(mesa_engine* eng, eng_peer* per_u, mesa_noun pax, mesa_noun pay)
{
  mesa_noun nul = mesa_cell(mesa_atom_u64(0), mesa_atom_u64(0));
  mesa_noun dap = mesa_at(6, pax);    //  /gk/agent
  mesa_noun req = mesa_at(3, pay);    //  [%0 path]
  c3_c*     dap_c;
  c3_c*     req_c;
  c3_c*     inn_c;
  c3_d      kid_d = 0;
  eng_shut* shu_u;

  if (  !eng->hok_u.shut_key
     || (MESA_NONE == dap) || !mesa_is_atom(dap)
     || !mesa_met3(dap) || (mesa_met3(dap) > 64)
     || !mesa_is_cell(pay) || (0 != mesa_head(pay))
     || (MESA_NONE == req) )
  {
    return nul;
  }

  //  //1/spur: only version 1
  //
  if (  !mesa_is_cell(req) || (0 != mesa_head(req))
     || !mesa_is_cell(mesa_tail(req))
     || (c3n == _cord_is(mesa_head(mesa_tail(req)), "1"))
     || !(req_c = eng_path_text(mesa_tail(mesa_tail(req)))) )
  {
    return nul;
  }
  {
    knot_path pax_u;
    if ( c3n == knot_path_split(strlen(req_c), req_c, &pax_u) ) {
      c3_free(req_c);
      return nul;
    }
  }
  dap_c = mesa_atom_get_cord(dap);

  {
    mesa_ship her = _her(per_u);
    eng->hok_o = c3y;
    kid_d = eng->hok_u.shut_key(eng->hok_u.ctx, her, dap_c, req_c);
    eng->hok_o = c3n;
  }

  //  the key must cover what they asked for
  //
  inn_c = c3_malloc(strlen(dap_c) + strlen(req_c) + 16);
  sprintf(inn_c, "/g/x/0/%s//1%s", dap_c, ( '/' == req_c[0] && !req_c[1] ) ? "" : req_c);
  shu_u = kid_d ? eng_shut_get(eng, kid_d) : 0;
  if ( shu_u && (c3n == eng_shut_under(shu_u->pre_c, inn_c)) ) {
    mesa_log("mesa: shut_key gave key %" PRIu64 " for %s, which it "
             "doesn't cover", kid_d, inn_c);
    shu_u = 0;
  }
  c3_free(inn_c);
  c3_free(dap_c);
  c3_free(req_c);

  if ( !shu_u ) {
    return nul;
  }
  mesa_unref(nul);
  return mesa_cell(mesa_atom_u64(0),
                   mesa_cell(mesa_atom_u64(0),
                             mesa_cell(mesa_ref(req),
                                       mesa_trel(mesa_atom_u64(1),
                                                 mesa_atom_u64(shu_u->kid_d),
                                                 mesa_atom_bytes(32, shu_u->key_y)))));
}

c3_o
eng_sys_plea(mesa_engine* eng, eng_peer* per_u, eng_flow* flo_u,
             mesa_noun van, mesa_noun pax, mesa_noun pay)
{
  if (  (c3n == _cord_is(van, "g"))
     || !mesa_is_cell(pax) )
  {
    return c3n;
  }

  //  /gf: they want to hear about our flubs; we tell them on this flow
  //
  if ( c3y == _cord_is(mesa_head(pax), "gf") ) {
    c3_y key_y[40];
    c3_z key_z = eng_key(KEY_GALL, per_u->who_u, 0, 0, 0, key_y);

    per_u->gbo_o = c3y;
    per_u->gbb_d = flo_u->bon_d;
    eng_put(eng, key_y, key_z, mesa_atom_u64(flo_u->bon_d));
    eng_flow_done(eng, per_u, flo_u, MESA_NONE);
    return c3y;
  }

  //  /gk/agent: a key for one of our %shut paths, if the application
  //  gives one; always an answer, or their scry would hang
  //
  if ( c3y == _cord_is(mesa_head(pax), "gk") ) {
    c3_d bon_d = flo_u->bon_d;
    mesa_noun bod = _brood(eng, per_u, pax, pay);

    if ( !(flo_u = eng_flow_get(per_u, bon_d, DIRE_BAK)) ) {
      mesa_unref(bod);
      return c3y;
    }
    eng_flow_pump(eng, per_u, flo_u,
                  mesa_cell(mesa_atom_cord("boon"), bod));
    eng_flow_done(eng, per_u, flo_u, MESA_NONE);
    return c3y;
  }
  return c3n;
}

/* _gall_boon(): a %boon to [per_u] on their /gf flow, if they have one
*/
static void
_gall_boon(mesa_engine* eng, eng_peer* per_u, mesa_noun res)
{
  eng_flow* gfl_u;

  if (  (c3n == per_u->gbo_o)
     || !(gfl_u = eng_flow_get(per_u, per_u->gbb_d, DIRE_BAK)) )
  {
    mesa_unref(res);
    return;
  }
  eng_flow_pump(eng, per_u, gfl_u,
                mesa_cell(mesa_atom_cord("boon"),
                          mesa_cell(mesa_atom_u64(0), res)));
}

/* eng_sys_flub(): +mo-do-flub; the application won't take a gall
**                 request on [flo_u] now: halt the flow until mesa_spur(),
**                 and tell them to stop sending. c3n if not a gall request.
*/
c3_o
eng_sys_flub(mesa_engine* eng, eng_peer* per_u, eng_flow* flo_u,
             mesa_noun van, mesa_noun pax)
{
  mesa_noun dap;

  //  [%g /ge/agent ...]
  //
  if (  (c3n == _cord_is(van, "g"))
     || !mesa_is_cell(pax)
     || (c3n == _cord_is(mesa_head(pax), "ge"))
     || !mesa_is_cell(mesa_tail(pax))
     || !mesa_is_atom(dap = mesa_head(mesa_tail(pax)))
     || !mesa_met3(dap) || (mesa_met3(dap) > 64)
     || (c3y == eng_sys_bone(per_u, flo_u->bon_d)) )
  {
    return c3n;
  }

  if ( MESA_NONE != flo_u->hag ) {
    mesa_unref(flo_u->hag);
  }
  flo_u->hat_o = c3y;
  flo_u->hag   = mesa_ref(dap);
  eng_flow_save(eng, per_u, flo_u);

  //  [%flub agent bone]: the bone they sent it on
  //
  _gall_boon(eng, per_u, mesa_trel(mesa_atom_cord("flub"), mesa_ref(dap),
                                   mesa_atom_u64(flo_u->bon_d)));
  return c3y;
}

/* eng_sys_spur(): +mo-give-halts; [dap_c] is back, resume its flows
*/
void
eng_sys_spur(mesa_engine* eng, const c3_c* dap_c)
{
  for ( eng_peer_map_itr pit_u = eng_peer_map_first(&eng->per_u);
        !eng_peer_map_is_end(pit_u);
        pit_u = eng_peer_map_next(pit_u) )
  {
    eng_peer* per_u = pit_u.data->val;
    c3_o      any_o = c3n;

    for ( eng_flow_map_itr itr_u = eng_flow_map_first(&per_u->flo_u);
          !eng_flow_map_is_end(itr_u);
          itr_u = eng_flow_map_next(itr_u) )
    {
      eng_flow* flo_u = itr_u.data->val;

      if (  (DIRE_BAK == flo_u->dir_e) && (MESA_NONE != flo_u->hag)
         && (c3y == _cord_is(flo_u->hag, dap_c)) )
      {
        mesa_unref(flo_u->hag);
        flo_u->hag = MESA_NONE;
        eng_flow_unhalt(eng, per_u, flo_u);
        any_o = c3y;
      }
    }

    //  [%spur agent]: they resend what we flubbed
    //
    if ( c3y == any_o ) {
      _gall_boon(eng, per_u, mesa_cell(mesa_atom_cord("spur"),
                                       mesa_atom_cord(dap_c)));
    }
  }
}

/* eng_sys_boon(): a boon on one of our system flows
*/
c3_o
eng_sys_boon(mesa_engine* eng, eng_peer* per_u, c3_d bon_d, mesa_noun pay)
{
  if ( (c3y == per_u->gfo_o) && (bon_d == per_u->gfb_d) ) {
    if ( mesa_is_cell(pay) && (0 == mesa_head(pay)) ) {
      _flub(eng, per_u, pay);
    }
    return c3y;
  }
  if ( (c3y == per_u->jao_o) && (bon_d == per_u->jab_d) ) {
    if ( mesa_is_cell(pay) ) {
      _keys(eng, per_u, pay);
    }
    return c3y;
  }
  return c3n;
}

/* eng_sys_done(): a system plea was (n)acked
*/
c3_o
eng_sys_done(mesa_engine* eng, eng_peer* per_u, c3_d bon_d, mesa_noun err)
{
  if ( (c3y == per_u->gfo_o) && (bon_d == per_u->gfb_d) ) {
    //  they didn't take it: track again on our next request
    //
    if ( MESA_NONE != err ) {
      per_u->gfo_o = c3n;
      eng_peer_save(eng, per_u);
    }
    return c3y;
  }
  if ( (c3y == per_u->jao_o) && (bon_d == per_u->jab_d) ) {
    if ( MESA_NONE != err ) {
      mesa_log("mesa: %s won't give us its moons' keys",
               eng_ship_text(per_u->who_u));
      _unask(eng, per_u);
    }
    return c3y;
  }
  return c3n;
}

/* eng_sys_breach(): [per_u] breached; its system flows are gone
*/
void
eng_sys_breach(mesa_engine* eng, eng_peer* per_u)
{
  if ( c3y == per_u->jao_o ) {
    _unask(eng, per_u);
  }
  if ( c3y == per_u->gbo_o ) {
    c3_y key_y[40];
    eng_del(eng, key_y, eng_key(KEY_GALL, per_u->who_u, 0, 0, 0, key_y));
  }
  per_u->gfo_o = c3n;
  per_u->jao_o = c3n;
  per_u->gbo_o = c3n;
}

/* points
*/

/* eng_point_apply(): +sy-publ; new keys, rift or sponsor for a peer
*/
void
eng_point_apply(mesa_engine* eng, const mesa_point* pot)
{
  u3_ship   who_u = { pot->who.lo, pot->who.hi };
  eng_peer* per_u;

  if ( c3y == u3_ships_equal(who_u, eng->our_u) ) {
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
    return;
  }

  if ( c3y == per_u->kno_o ) {
    if ( pot->rift > per_u->rif_d ) {
      eng_peer_breach(eng, per_u);
      eng_peer_set_keys(eng, per_u, pot->rift, pot->life, pot->pass);
      eng_peer_save(eng, per_u);
      if ( eng->hok_u.breach ) {
        eng->hok_o = c3y;
        eng->hok_u.breach(eng->hok_u.ctx, _her(per_u));
        eng->hok_o = c3n;
      }
    }
    else if (  (pot->life != per_u->lyf_d)
            || memcmp(pot->pass + 1, per_u->sgn_y, 32)
            || memcmp(pot->pass + 33, per_u->cry_y, 32) )
    {
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
}

/// @file
///
/// Flows: |fo in ames.hoon, plus +hear-poke:ev-pact and +ev-take-sage.

#include "eng.h"

#define SIDE(bon_d, dir_e)  (((bon_d) << 1) | (c3_d)(dir_e))

static eng_dire
_flip(eng_dire dir_e)
{
  return ( DIRE_FOR == dir_e ) ? DIRE_BAK : DIRE_FOR;
}

static const c3_c*
_dire_text(eng_dire dir_e)
{
  return ( DIRE_FOR == dir_e ) ? "for" : "bak";
}

static c3_o
_is_ping(mesa_engine* eng, eng_peer* per_u, c3_d bon_d)
{
  return __(  (c3y == eng->pin_o) && (bon_d == eng->pib_d)
           && (c3y == u3_ships_equal(per_u->who_u, eng->gal_u)) );
}

static mesa_ship
_her(eng_peer* per_u)
{
  return (mesa_ship){ .lo = per_u->who_u[0], .hi = per_u->who_u[1] };
}

/* lifecycle
*/
eng_flow*
eng_flow_get(eng_peer* per_u, c3_d bon_d, eng_dire dir_e)
{
  eng_flow_map_itr itr_u = eng_flow_map_get(&per_u->flo_u, SIDE(bon_d, dir_e));
  return eng_flow_map_is_end(itr_u) ? 0 : itr_u.data->val;
}

eng_flow*
eng_flow_make(mesa_engine* eng, eng_peer* per_u, c3_d bon_d, eng_dire dir_e)
{
  eng_flow* flo_u = eng_flow_get(per_u, bon_d, dir_e);

  if ( flo_u ) {
    return flo_u;
  }

  flo_u = c3_calloc(sizeof(*flo_u));
  flo_u->bon_d = bon_d;
  flo_u->dir_e = dir_e;
  flo_u->clo_o = c3n;
  flo_u->hat_o = c3n;
  flo_u->hag   = MESA_NONE;
  flo_u->nex_d = 1;
  flo_u->win_w = 1;
  flo_u->pen_o = c3n;

  if ( eng_flow_map_is_end(eng_flow_map_insert(&per_u->flo_u,
                                               SIDE(bon_d, dir_e), flo_u)) )
  {
    mesa_fail(__FILE__, __LINE__, "out of memory");
  }
  return flo_u;
}

void
eng_flow_free(eng_flow* flo_u)
{
  while ( flo_u->lod_u ) {
    eng_msg* nex_u = flo_u->lod_u->nex_u;
    mesa_unref(flo_u->lod_u->msg);
    c3_free(flo_u->lod_u);
    flo_u->lod_u = nex_u;
  }
  for ( c3_w i = 0; i < flo_u->nan_w; i++ ) {
    mesa_unref(flo_u->nax_u[i].err);
  }
  if ( MESA_NONE != flo_u->hag ) {
    mesa_unref(flo_u->hag);
  }
  c3_free(flo_u);
}

/* corks
**
**   Ours (forward) are derived: a bone of ours below next-bone without a
**   flow was corked. Theirs (backward) are ranges, stored one row per
**   range: 'C' ship lo %bak -> hi.
*/

/* _cork_find(): the index of the last range with lo <= [bon_d], or -1
*/
static c3_ds
_cork_find(eng_crk* crk_u, c3_d bon_d)
{
  c3_ds lo_ds = 0, hi_ds = (c3_ds)crk_u->len_w - 1, fon_ds = -1;

  while ( lo_ds <= hi_ds ) {
    c3_ds mid_ds = (lo_ds + hi_ds) / 2;
    if ( crk_u->ran_u[mid_ds].lo_d <= bon_d ) {
      fon_ds = mid_ds;
      lo_ds  = mid_ds + 1;
    }
    else {
      hi_ds = mid_ds - 1;
    }
  }
  return fon_ds;
}

c3_o
eng_flow_corked(eng_peer* per_u, c3_d bon_d, eng_dire dir_e)
{
  if ( DIRE_FOR == dir_e ) {
    return __(  (0 == (bon_d & 3)) && (bon_d < per_u->bon_d)
             && !eng_flow_get(per_u, bon_d, DIRE_FOR) );
  }
  {
    eng_crk* crk_u = &per_u->crk_u[bon_d & 3];
    c3_ds    idx_ds = _cork_find(crk_u, bon_d);
    return __( (idx_ds >= 0) && (bon_d <= crk_u->ran_u[idx_ds].hi_d) );
  }
}

static void
_cork_row(mesa_engine* eng, eng_peer* per_u, eng_ran* ran_u)
{
  c3_y key_y[40];
  c3_z key_z = eng_key(KEY_CORK, per_u->who_u, ran_u->lo_d, DIRE_BAK, 0, key_y);
  eng_put(eng, key_y, key_z, mesa_atom_u64(ran_u->hi_d));
}

static void
_cork_unrow(mesa_engine* eng, eng_peer* per_u, c3_d lo_d)
{
  c3_y key_y[40];
  eng_del(eng, key_y, eng_key(KEY_CORK, per_u->who_u, lo_d, DIRE_BAK, 0, key_y));
}

/* eng_cork_load(): a range from the store (or an old one-bone row)
*/
void
eng_cork_load(eng_peer* per_u, c3_d lo_d, c3_d hi_d)
{
  eng_crk* crk_u = &per_u->crk_u[lo_d & 3];
  c3_ds    idx_ds;

  if ( (hi_d < lo_d) || ((hi_d - lo_d) & 3) ) {
    hi_d = lo_d;
  }
  idx_ds = _cork_find(crk_u, lo_d);
  if ( crk_u->len_w == crk_u->cap_w ) {
    crk_u->cap_w = c3_max(4, crk_u->cap_w * 2);
    crk_u->ran_u = c3_realloc(crk_u->ran_u, crk_u->cap_w * sizeof(eng_ran));
  }
  memmove(&crk_u->ran_u[idx_ds + 2], &crk_u->ran_u[idx_ds + 1],
          (crk_u->len_w - (idx_ds + 1)) * sizeof(eng_ran));
  crk_u->ran_u[idx_ds + 1] = (eng_ran){ lo_d, hi_d };
  crk_u->len_w++;
}

/* eng_cork_put(): remember their flow [bon_d] as corked, merging ranges;
**                 [sav_o] writes the store
*/
void
eng_cork_put(mesa_engine* eng, eng_peer* per_u, c3_d bon_d, c3_o sav_o)
{
  eng_crk* crk_u = &per_u->crk_u[bon_d & 3];
  c3_ds    idx_ds = _cork_find(crk_u, bon_d);
  eng_ran* lef_u = ( idx_ds >= 0 ) ? &crk_u->ran_u[idx_ds] : 0;
  eng_ran* rit_u = ( (c3_w)(idx_ds + 1) < crk_u->len_w )
                   ? &crk_u->ran_u[idx_ds + 1] : 0;

  if ( lef_u && (bon_d <= lef_u->hi_d) ) {
    return;   //  already
  }
  if ( lef_u && (lef_u->hi_d + 4 != bon_d) ) lef_u = 0;
  if ( rit_u && (bon_d + 4 != rit_u->lo_d) ) rit_u = 0;

  if ( lef_u && rit_u ) {
    lef_u->hi_d = rit_u->hi_d;
    if ( c3y == sav_o ) {
      _cork_unrow(eng, per_u, rit_u->lo_d);
      _cork_row(eng, per_u, lef_u);
    }
    memmove(rit_u, rit_u + 1,
            (crk_u->len_w - (idx_ds + 2)) * sizeof(eng_ran));
    crk_u->len_w--;
  }
  else if ( lef_u ) {
    lef_u->hi_d = bon_d;
    if ( c3y == sav_o ) _cork_row(eng, per_u, lef_u);
  }
  else if ( rit_u ) {
    if ( c3y == sav_o ) _cork_unrow(eng, per_u, rit_u->lo_d);
    rit_u->lo_d = bon_d;
    if ( c3y == sav_o ) _cork_row(eng, per_u, rit_u);
  }
  else {
    eng_cork_load(per_u, bon_d, bon_d);
    if ( c3y == sav_o ) {
      eng_ran ran_u = { bon_d, bon_d };
      _cork_row(eng, per_u, &ran_u);
    }
  }
}

c3_w
eng_cork_count(eng_peer* per_u)
{
  c3_w len_w = 0;
  for ( c3_w i = 0; i < 4; i++ ) len_w += per_u->crk_u[i].len_w;
  return len_w;
}

/* eng_cork_wipe(): forget all their corks (a breach), and our old
**                  per-bone rows
*/
void
eng_cork_wipe(mesa_engine* eng, eng_peer* per_u)
{
  c3_y pre_y[40];

  eng_key(KEY_CORK, per_u->who_u, 0, 0, 0, pre_y);
  eng_del_prefix(eng, pre_y, 17);
  for ( c3_w i = 0; i < 4; i++ ) {
    c3_free(per_u->crk_u[i].ran_u);
    per_u->crk_u[i] = (eng_crk){ 0 };
  }
}

/* eng_flow_save(): persist a flow's header
**
**   [closing halt next window last-acked pending nax]
**
**   [halt] is a loobean, or for a backward flow we flubbed, the agent.
*/
void
eng_flow_save(mesa_engine* eng, eng_peer* per_u, eng_flow* flo_u)
{
  c3_y      key_y[40];
  c3_z      key_z = eng_key(KEY_FLOW, per_u->who_u, flo_u->bon_d,
                            flo_u->dir_e, 0, key_y);
  mesa_noun nax = mesa_atom_u64(0);

  for ( c3_w i = flo_u->nan_w; i-- > 0; ) {
    nax = mesa_cell(mesa_cell(mesa_atom_u64(flo_u->nax_u[i].seq_d),
                              mesa_ref(flo_u->nax_u[i].err)),
                    nax);
  }

  eng_put(eng, key_y, key_z,
          mesa_cell(mesa_qual(mesa_atom_u64(c3y == flo_u->clo_o),
                              ( MESA_NONE != flo_u->hag )
                              ? mesa_ref(flo_u->hag)
                              : mesa_atom_u64(c3y == flo_u->hat_o),
                              mesa_atom_u64(flo_u->nex_d),
                              mesa_atom_u64(flo_u->win_w)),
                    mesa_trel(mesa_atom_u64(flo_u->las_d),
                              mesa_atom_u64(c3y == flo_u->pen_o),
                              nax)));
}

/* _flow_abel(): +fo-abel; the flow is gone, remember it as corked
*/
static void
_flow_abel(mesa_engine* eng, eng_peer* per_u, eng_flow* flo_u)
{
  c3_y     key_y[40];
  c3_d     bon_d = flo_u->bon_d;
  eng_dire dir_e = flo_u->dir_e;
  eng_req* req_u = per_u->req_u;

  //  drop requests for this flow
  //
  while ( req_u ) {
    eng_req* nex_u = req_u->nex_u;
    if (  (REQ_KEEN != req_u->typ_e) && (REQ_PROOF != req_u->typ_e)
       && (req_u->bon_d == bon_d) && (req_u->dir_e == dir_e) )
    {
      eng_req_del(per_u, req_u);
      eng_req_free(req_u);
    }
    req_u = nex_u;
  }

  for ( eng_msg* msg_u = flo_u->lod_u; msg_u; msg_u = msg_u->nex_u ) {
    eng_del(eng, key_y, eng_key(KEY_LOAD, per_u->who_u, bon_d, dir_e,
                                msg_u->seq_d, key_y));
  }
  eng_del(eng, key_y, eng_key(KEY_FLOW, per_u->who_u, bon_d, dir_e, 0, key_y));

  eng_flow_map_erase(&per_u->flo_u, SIDE(bon_d, dir_e));
  eng_flow_free(flo_u);

  //  ours are corked by being gone; theirs we remember
  //
  if ( DIRE_BAK == dir_e ) {
    eng_cork_put(eng, per_u, bon_d, c3y);
  }
}

/* paths
*/
c3_c*
eng_flow_path(c3_d bon_d, const c3_c* lod_c, eng_dire dir_e,
              u3_ship srv_u, c3_d seq_d)
{
  c3_c  bon_c[32], seq_c[32];
  c3_c* who_c = u3_ship_to_string(srv_u);
  c3_c* out_c = c3_malloc(strlen(who_c) + 128);

  knot_ud(bon_d, bon_c);
  if ( 0 == strcmp(lod_c, "cork") ) {
    sprintf(out_c, "/a/x/1//flow/%s/cork/%s/%s", bon_c, _dire_text(dir_e), who_c);
  }
  else {
    knot_ud(seq_d, seq_c);
    sprintf(out_c, "/a/x/1//flow/%s/%s/%s/%s/%s",
            bon_c, lod_c, _dire_text(dir_e), who_c, seq_c);
  }
  c3_free(who_c);
  return out_c;
}

/* _space(): an inner path in their (c3n) or our (c3y) namespace
*/
static c3_c*
_space(mesa_engine* eng, eng_peer* per_u, c3_o our_o, c3_c* inn_c)
{
  c3_c* pat_c = eng_chum_path(eng, per_u, our_o, inn_c);
  c3_free(inn_c);
  return pat_c;
}

/* +fo-ack-path etc. All of these name payloads on the other side, so
** the dire in the path is flipped from ours.
*/
static c3_c*
_their_ack(mesa_engine* eng, eng_peer* per_u, eng_flow* flo_u, c3_d seq_d)
{
  return _space(eng, per_u, c3n,
                eng_flow_path(flo_u->bon_d, "ack", _flip(flo_u->dir_e),
                              eng->our_u, seq_d));
}

static c3_c*
_their_naxp(mesa_engine* eng, eng_peer* per_u, eng_flow* flo_u, c3_d seq_d)
{
  return _space(eng, per_u, c3n,
                eng_flow_path(flo_u->bon_d, "naxp", _flip(flo_u->dir_e),
                              eng->our_u, seq_d));
}

static c3_c*
_their_cork(mesa_engine* eng, eng_peer* per_u, eng_flow* flo_u)
{
  return _space(eng, per_u, c3n,
                eng_flow_path(flo_u->bon_d, "cork", _flip(flo_u->dir_e),
                              eng->our_u, 0));
}

//  ours, for them to read
//
static c3_c*
_our_poke(mesa_engine* eng, eng_peer* per_u, eng_flow* flo_u, c3_d seq_d)
{
  return _space(eng, per_u, c3y,
                eng_flow_path(flo_u->bon_d, "poke", flo_u->dir_e,
                              per_u->who_u, seq_d));
}

static c3_c*
_our_ack(mesa_engine* eng, eng_peer* per_u, eng_flow* flo_u, c3_d seq_d)
{
  return _space(eng, per_u, c3y,
                eng_flow_path(flo_u->bon_d, "ack", flo_u->dir_e,
                              per_u->who_u, seq_d));
}

/* sending
*/
static eng_req*
_flow_req(mesa_engine* eng, eng_peer* per_u, eng_flow* flo_u,
          c3_c* pat_c, eng_req_e typ_e, c3_d seq_d)
{
  eng_req* req_u;

  if ( !pat_c ) {
    return 0;
  }
  if ( (req_u = eng_req_find(per_u, pat_c)) ) {
    c3_free(pat_c);
    return 0;
  }
  req_u = eng_req_add(eng, per_u, pat_c, typ_e);
  req_u->bon_d = flo_u->bon_d;
  req_u->dir_e = flo_u->dir_e;
  req_u->seq_d = seq_d;
  c3_free(pat_c);
  return req_u;
}

/* _flow_poke(): start the %poke for our oldest unacked message
*/
static void
_flow_poke(mesa_engine* eng, eng_peer* per_u, eng_flow* flo_u)
{
  eng_msg* msg_u = flo_u->lod_u;
  eng_req* req_u;

  if ( !msg_u || (c3n == per_u->kno_o) ) {
    return;
  }

  req_u = _flow_req(eng, per_u, flo_u, _their_ack(eng, per_u, flo_u, msg_u->seq_d),
                    REQ_ACK, msg_u->seq_d);
  if ( req_u ) {
    req_u->pay_c = _our_poke(eng, per_u, flo_u, msg_u->seq_d);
    eng_req_send(eng, per_u, req_u);
  }
}

/* eng_flow_send(): +fo-send, with a send-window of one
*/
void
eng_flow_send(mesa_engine* eng, eng_peer* per_u, eng_flow* flo_u)
{
  if ( (0 == flo_u->win_w) || !flo_u->lod_u || (c3n == per_u->kno_o) ) {
    return;
  }
  flo_u->win_w--;
  eng_flow_save(eng, per_u, flo_u);
  _flow_poke(eng, per_u, flo_u);
}

static c3_o
_is_cork_plea(mesa_noun msg)
{
  //  [%plea %$ /flow %cork ~]
  //
  mesa_noun pax = mesa_at(14, msg);
  mesa_noun pay = mesa_at(15, msg);
  c3_d      van_d;
  char*     hed_c;
  c3_o      ret_o = c3n;

  if (  (MESA_NONE == pax) || (MESA_NONE == pay)
     || !mesa_atom_get_u64(mesa_at(6, msg), &van_d) || van_d
     || !mesa_is_cell(pax) || !mesa_is_cell(pay) )
  {
    return c3n;
  }
  hed_c = mesa_atom_get_cord(mesa_head(pax));
  if ( (0 == strcmp(hed_c, "flow")) && (0 == mesa_tail(pax)) ) {
    char* cor_c = mesa_atom_get_cord(mesa_head(pay));
    ret_o = __( (0 == strcmp(cor_c, "cork")) && (0 == mesa_tail(pay)) );
    c3_free(cor_c);
  }
  c3_free(hed_c);
  return ret_o;
}

/* eng_flow_append(): add [msg_u] to the end of the queue
*/
void
eng_flow_append(eng_flow* flo_u, eng_msg* msg_u)
{
  msg_u->nex_u = 0;
  if ( flo_u->lot_u ) flo_u->lot_u->nex_u = msg_u;
  else                flo_u->lod_u = msg_u;
  flo_u->lot_u = msg_u;
}

/* eng_flow_pump(): +pump, queue an outbound message (transfers [msg])
*/
void
eng_flow_pump(mesa_engine* eng, eng_peer* per_u, eng_flow* flo_u,
              mesa_noun msg)
{
  eng_msg*  msg_u = c3_calloc(sizeof(*msg_u));
  c3_y      key_y[40];
  c3_z      key_z;

  msg_u->seq_d = flo_u->nex_d++;
  msg_u->msg   = msg;
  eng_flow_append(flo_u, msg_u);

  key_z = eng_key(KEY_LOAD, per_u->who_u, flo_u->bon_d, flo_u->dir_e,
                  msg_u->seq_d, key_y);
  eng_put(eng, key_y, key_z, mesa_ref(msg));
  eng_flow_save(eng, per_u, flo_u);

  if ( c3n == flo_u->hat_o ) {
    eng_flow_send(eng, per_u, flo_u);
  }
}

/* serving: +fo-peek
*/
mesa_noun
eng_flow_peek(mesa_engine* eng, eng_peer* per_u, c3_d bon_d,
              eng_dire dir_e, const c3_c* lod_c, c3_d seq_d)
{
  eng_flow* flo_u = eng_flow_get(per_u, bon_d, dir_e);
  c3_o      cor_o = eng_flow_corked(per_u, bon_d, dir_e);
  c3_d      las_d = flo_u ? flo_u->las_d : 0;

  if ( 0 == strcmp(lod_c, "naxp") ) {
    for ( c3_w i = 0; flo_u && (i < flo_u->nan_w); i++ ) {
      if ( flo_u->nax_u[i].seq_d == seq_d ) {
        return mesa_cell(mesa_atom_cord("nax"), mesa_ref(flo_u->nax_u[i].err));
      }
    }
    return MESA_NONE;
  }

  if ( 0 == strcmp(lod_c, "cork") ) {
    return ( c3y == cor_o )
           ? mesa_cell(mesa_atom_cord("gone"), mesa_atom_u64(0))
           : MESA_NONE;
  }

  if ( 0 == strcmp(lod_c, "poke") ) {
    for ( eng_msg* msg_u = flo_u ? flo_u->lod_u : 0; msg_u; msg_u = msg_u->nex_u ) {
      if ( msg_u->seq_d == seq_d ) {
        return mesa_ref(msg_u->msg);
      }
    }
    return MESA_NONE;
  }

  if ( 0 == strcmp(lod_c, "ack") ) {
    c3_o nak_o = c3n;

    //  a corked publisher acks the %cork
    //
    if ( (c3y == cor_o) && (DIRE_BAK == dir_e) ) {
      goto ack;
    }
    //  boons are always acked
    //
    if ( DIRE_FOR == dir_e ) {
      if ( seq_d > las_d ) return MESA_NONE;
      goto ack;
    }
    for ( c3_w i = 0; flo_u && (i < flo_u->nan_w); i++ ) {
      if ( flo_u->nax_u[i].seq_d == seq_d ) {
        nak_o = c3y;
        goto ack;
      }
    }
    if ( (seq_d < las_d) && ((las_d - seq_d) >= 10) ) return MESA_NONE;
    if ( seq_d > las_d ) return MESA_NONE;

  ack:
    //  [%ack error=?]: loobeans, so %.n (1) is a positive ack
    //
    return mesa_cell(mesa_atom_cord("ack"), mesa_atom_u64(( c3y == nak_o ) ? 0 : 1));
  }

  return MESA_NONE;
}

/* _send_ack(): +fo-send-ack, push our (n)ack page for [seq_d]
*/
static void
_send_ack(mesa_engine* eng, eng_peer* per_u, eng_flow* flo_u, c3_d seq_d)
{
  c3_c* pat_c = _our_ack(eng, per_u, flo_u, seq_d);

  if ( pat_c ) {
    eng_give_page(eng, per_u, pat_c);
    c3_free(pat_c);
  }
}

/* eng_flow_done(): +fo-take-done, finish the pending plea
*/
void
eng_flow_done(mesa_engine* eng, eng_peer* per_u, eng_flow* flo_u,
              mesa_noun err)
{
  c3_d seq_d = flo_u->las_d + 1;

  flo_u->las_d = seq_d;
  flo_u->pen_o = c3n;

  if ( MESA_NONE != err ) {
    //  keep the last ten
    //
    if ( 10 == flo_u->nan_w ) {
      mesa_unref(flo_u->nax_u[0].err);
      memmove(flo_u->nax_u, flo_u->nax_u + 1, 9 * sizeof(eng_nax));
      flo_u->nan_w--;
    }
    flo_u->nax_u[flo_u->nan_w].seq_d = seq_d;
    flo_u->nax_u[flo_u->nan_w].err   = err;
    flo_u->nan_w++;
  }

  eng_flow_save(eng, per_u, flo_u);
  _send_ack(eng, per_u, flo_u, seq_d);
}

/* _peek_cork(): +fo-peek-cork, see whether they've closed their side
*/
static void
_peek_cork(mesa_engine* eng, eng_peer* per_u, eng_flow* flo_u)
{
  eng_req* req_u = _flow_req(eng, per_u, flo_u, _their_cork(eng, per_u, flo_u),
                             REQ_CORK, 0);
  if ( req_u ) {
    eng_req_send(eng, per_u, req_u);
  }
}

static void
_peek_naxp(mesa_engine* eng, eng_peer* per_u, eng_flow* flo_u, c3_d seq_d)
{
  eng_req* req_u = _flow_req(eng, per_u, flo_u,
                             _their_naxp(eng, per_u, flo_u, seq_d),
                             REQ_NAXP, seq_d);
  if ( req_u ) {
    eng_req_send(eng, per_u, req_u);
  }
}

/* _default_error(): [%error ~]
*/
static mesa_noun
_default_error(void)
{
  return mesa_cell(mesa_atom_cord("error"), mesa_atom_u64(0));
}

/* _sink_plea(): +fo-sink-plea; hand a request to the application
*/
static void
_sink_plea(mesa_engine* eng, eng_peer* per_u, eng_flow* flo_u, mesa_noun pag)
{
  mesa_noun van, pax, pay;
  c3_d      van_d;

  //  [%plea vane path payload]
  //
  if (  !mesa_is_cell(mesa_tail(pag))
     || !mesa_is_cell(mesa_at(7, pag))
     || !mesa_is_atom(van = mesa_at(6, pag)) )
  {
    return;
  }
  pax = mesa_at(14, pag);
  pay = mesa_at(15, pag);

  if ( c3y == flo_u->pen_o ) {
    return;
  }
  flo_u->pen_o = c3y;

  if ( mesa_atom_get_u64(van, &van_d) && (0 == van_d) ) {
    if ( c3y == _is_cork_plea(pag) ) {
      if ( 0 == flo_u->las_d ) {
        //  a %cork as the first message: drop it
        //
        flo_u->pen_o = c3n;
        return;
      }
      flo_u->clo_o = c3y;
      eng_flow_done(eng, per_u, flo_u, MESA_NONE);
      _peek_cork(eng, per_u, flo_u);
      return;
    }

    //  %ahoy (migration) is always acked; anything else we don't do
    //
    if ( mesa_is_cell(pay) && mesa_is_atom(mesa_head(pay)) ) {
      char* tag_c = mesa_atom_get_cord(mesa_head(pay));
      c3_o  aho_o = __(0 == strcmp(tag_c, "ahoy"));
      c3_free(tag_c);
      if ( c3y == aho_o ) {
        eng_flow_done(eng, per_u, flo_u, MESA_NONE);
        return;
      }
    }
    eng_flow_done(eng, per_u, flo_u, _default_error());
    return;
  }

  //  vanes' own business (gall's /gf and /gk)
  //
  if ( c3y == eng_sys_plea(eng, per_u, flo_u, van, pax, pay) ) {
    return;
  }

  //  no one to take it: don't answer
  //
  if ( !eng->hok_u.plea ) {
    flo_u->pen_o = c3n;
    return;
  }

  //  run the handler inside a savepoint, so a nack undoes its writes
  //
  {
    mesa_store*      sto_u = eng->sto_u;
    eng_out**        mar_u = eng->ont_u;
    c3_d             api_d = eng->api_d;
    c3_o             sav_o = __(sto_u->mark && !sto_u->mark(sto_u->ctx));
    mesa_noun        err   = MESA_NONE;
    mesa_plea_result res_e;
    c3_d             bon_d = flo_u->bon_d;

    eng->hok_o = c3y;
    res_e = eng->hok_u.plea(eng->hok_u.ctx, _her(per_u), bon_d,
                            van, pax, pay, &err);
    eng->hok_o = c3n;

    //  the handler may have changed our flows; find ours again
    //
    flo_u = eng_flow_get(per_u, bon_d, DIRE_BAK);
    if ( !flo_u ) {
      mesa_unref(err);
      return;
    }

    switch ( res_e ) {
      case MESA_ACK: {
        if ( c3y == sav_o ) sto_u->release(sto_u->ctx);
        if ( MESA_NONE != err ) mesa_unref(err);
        eng_flow_done(eng, per_u, flo_u, MESA_NONE);
      } break;

      case MESA_LATER: {
        if ( c3y == sav_o ) sto_u->release(sto_u->ctx);
        if ( MESA_NONE != err ) mesa_unref(err);
        eng_flow_save(eng, per_u, flo_u);
      } break;

      case MESA_DROP:
      case MESA_FLUB: {
        //  no answer: undo the handler (with savepoints; without, it
        //  mustn't have written), and take the resend when it comes
        //
        if ( MESA_NONE != err ) mesa_unref(err);
        if ( c3y == sav_o ) {
          sto_u->rewind(sto_u->ctx);
          sto_u->release(sto_u->ctx);
          eng_out_cut(eng, mar_u);
          if ( api_d != eng->api_d ) {
            eng->rel_o = c3y;
          }
        }
        flo_u->pen_o = c3n;

        //  flub: halt the flow and tell them to stop sending, until
        //  mesa_spur(); a drop if it isn't a gall request
        //
        if ( MESA_FLUB == res_e ) {
          eng_sys_flub(eng, per_u, flo_u, van, pax);
        }
      } break;

      default: {
        if ( c3y == sav_o ) {
          sto_u->rewind(sto_u->ctx);
          sto_u->release(sto_u->ctx);
          eng_out_cut(eng, mar_u);
          if ( api_d != eng->api_d ) {
            //  the handler called us, and that's undone in the store;
            //  rebuild our memory after this event commits
            //
            eng->rel_o = c3y;
          }
        }
        else {
          //  no savepoints: we can't undo the handler; fail the event
          //
          eng_txn_fail(eng);
        }
        eng_flow_done(eng, per_u, flo_u,
                      ( MESA_NONE == err ) ? _default_error() : err);
      } break;
    }
  }
}

/* _sink_boon(): +fo-sink-boon; responses are always acked
*/
static void
_sink_boon(mesa_engine* eng, eng_peer* per_u, eng_flow* flo_u, mesa_noun pag)
{
  c3_d bon_d = flo_u->bon_d;

  if ( !mesa_is_cell(pag) ) {
    return;
  }

  if ( c3y == eng_sys_boon(eng, per_u, bon_d, mesa_tail(pag)) ) {
    //  ours, not the application's
  }
  else if ( eng->hok_u.boon && (c3n == _is_ping(eng, per_u, bon_d)) ) {
    eng->hok_o = c3y;
    eng->hok_u.boon(eng->hok_u.ctx, _her(per_u), bon_d, mesa_tail(pag));
    eng->hok_o = c3n;
  }

  if ( !(flo_u = eng_flow_get(per_u, bon_d, DIRE_FOR)) ) {
    return;
  }
  flo_u->las_d++;
  eng_flow_save(eng, per_u, flo_u);
  _send_ack(eng, per_u, flo_u, flo_u->las_d);
}

/* eng_flow_sink(): +sink, a complete message on flow [bon_d dir_e]
*/
void
eng_flow_sink(mesa_engine* eng, eng_peer* per_u, c3_d bon_d,
              eng_dire dir_e, c3_d seq_d, mesa_noun gag)
{
  eng_flow* flo_u;
  mesa_noun pag;
  c3_o      cor_o = eng_flow_corked(per_u, bon_d, dir_e);
  char*     tag_c;
  c3_o      mes_o;

  //  [%message page]
  //
  if ( !mesa_is_cell(gag) || !mesa_is_atom(mesa_head(gag)) ) {
    return;
  }
  tag_c = mesa_atom_get_cord(mesa_head(gag));
  mes_o = __(0 == strcmp(tag_c, "message"));
  c3_free(tag_c);
  pag = mesa_tail(gag);
  if ( (c3n == mes_o) || !mesa_is_cell(pag) ) {
    return;
  }

  //  a corked flow only acks the %cork (or a %leave) again
  //
  if ( c3y == cor_o ) {
    if ( (DIRE_BAK == dir_e) && (c3y == _is_cork_plea(pag)) ) {
      eng_flow tmp_u = { .bon_d = bon_d, .dir_e = dir_e };
      _send_ack(eng, per_u, &tmp_u, seq_d);
    }
    return;
  }

  if ( DIRE_FOR == dir_e ) {
    //  boons only arrive on flows we opened
    //
    if ( !(flo_u = eng_flow_get(per_u, bon_d, DIRE_FOR)) ) {
      return;
    }
  }
  else {
    flo_u = eng_flow_make(eng, per_u, bon_d, DIRE_BAK);
  }

  if ( (c3y == flo_u->hat_o) || (c3y == flo_u->clo_o) ) {
    //  closing: only a resent %cork gets its ack again
    //
    if ( c3y == flo_u->hat_o ) {
      return;
    }
    if (  (DIRE_BAK != dir_e) || (c3n == _is_cork_plea(pag))
       || (seq_d != flo_u->las_d) )
    {
      return;
    }
    _send_ack(eng, per_u, flo_u, seq_d);
    return;
  }

  if ( seq_d > flo_u->las_d + 1 ) {
    return;
  }

  if ( seq_d == flo_u->las_d + 1 ) {
    if ( DIRE_BAK == dir_e ) _sink_plea(eng, per_u, flo_u, pag);
    else                     _sink_boon(eng, per_u, flo_u, pag);
    return;
  }

  //  a resend: ack again, inside the window
  //
  if ( (flo_u->las_d - seq_d) >= 10 ) {
    return;
  }
  _send_ack(eng, per_u, flo_u, seq_d);
}

/* _peek_poke(): +fo-peek-poke, fetch a multi-fragment payload
*/
static void
_peek_poke(mesa_engine* eng, eng_peer* per_u, c3_d bon_d, eng_dire dir_e,
           c3_d seq_d, const c3_c* pok_c)
{
  eng_flow* flo_u = eng_flow_get(per_u, bon_d, dir_e);
  eng_req*  req_u;

  if ( c3y == eng_flow_corked(per_u, bon_d, dir_e) ) {
    return;
  }
  if ( DIRE_FOR == dir_e ) {
    if ( !flo_u ) return;   //  not a flow of ours
  }
  else {
    flo_u = eng_flow_make(eng, per_u, bon_d, dir_e);
  }

  if ( (c3y == flo_u->hat_o) || (c3y == flo_u->clo_o) ) {
    return;
  }

  if ( seq_d <= flo_u->las_d ) {
    if ( (flo_u->las_d - seq_d) < 10 ) {
      _send_ack(eng, per_u, flo_u, seq_d);
    }
    return;
  }

  if ( !eng_req_find(per_u, pok_c) ) {
    req_u = eng_req_add(eng, per_u, pok_c, REQ_POKE);
    req_u->bon_d = bon_d;
    req_u->dir_e = dir_e;
    req_u->seq_d = seq_d;
    eng_req_send(eng, per_u, req_u);
  }
}

/* _flow_pith(): parse /a/x/1//flow/[bone]/[load]/[dire]/[rcvr]/[seq]
*/
typedef struct {
  c3_d     bon_d;
  c3_c     lod_c[8];
  eng_dire dir_e;
  u3_ship  rcv_u;
  c3_d     seq_d;
} _pith;

static c3_o
_flow_pith(const c3_c* inn_c, _pith* pit_u)
{
  knot_path pax_u;
  knot_seg* seg_u = pax_u.seg_u;

  if (  (c3n == knot_path_split(strlen(inn_c), inn_c, &pax_u))
     || (10 != pax_u.len_w)
     || (c3n == knot_seg_is(&seg_u[0], "a"))
     || (c3n == knot_seg_is(&seg_u[1], "x"))
     || (c3n == knot_seg_is(&seg_u[2], "1"))
     || (0 != seg_u[3].len_z)
     || (c3n == knot_seg_is(&seg_u[4], "flow"))
     || (c3n == knot_ud_parse(seg_u[5].len_z, seg_u[5].str_c, &pit_u->bon_d))
     || (seg_u[6].len_z >= sizeof(pit_u->lod_c))
     || (c3n == knot_p_parse(seg_u[8].len_z, seg_u[8].str_c, pit_u->rcv_u))
     || (c3n == knot_ud_parse(seg_u[9].len_z, seg_u[9].str_c, &pit_u->seq_d)) )
  {
    return c3n;
  }
  memcpy(pit_u->lod_c, seg_u[6].str_c, seg_u[6].len_z);
  pit_u->lod_c[seg_u[6].len_z] = 0;

  if      ( c3y == knot_seg_is(&seg_u[7], "for") ) pit_u->dir_e = DIRE_FOR;
  else if ( c3y == knot_seg_is(&seg_u[7], "bak") ) pit_u->dir_e = DIRE_BAK;
  else return c3n;

  return c3y;
}

/* eng_hear_poke(): +hear-poke:ev-pact (and the %poke arm of +pe-heer)
*/
void
eng_hear_poke(mesa_engine* eng, mesa_lane lan_u, u3_mesa_pact* pac_u)
{
  u3_mesa_name* ack_u = &pac_u->pok_u.nam_u;
  u3_mesa_name* pok_u = &pac_u->pok_u.pay_u;
  u3_mesa_data* dat_u = &pac_u->pok_u.dat_u;
  eng_peer*     per_u;
  c3_c          *ack_c = 0, *pok_c = 0, *aci_c = 0, *poi_c = 0;
  _pith         ack_p, pok_p;
  c3_d          tof_d;
  c3_y          roo_y[32];

  if (  (c3n == u3_ships_equal(ack_u->her_u, eng->our_u))
     || (c3y == u3_ships_equal(pok_u->her_u, eng->our_u))
     || (ENG_BOQ != pok_u->boq_y) )
  {
    return;
  }

  //  strangers cost a key lookup each; don't let them pile up
  //
  if ( !(per_u = eng_peer_get(eng, pok_u->her_u)) ) {
    if ( (eng->ali_w >= ENG_ALIEN_MAX) && (c3n == eng_peer_evict(eng)) ) {
      return;
    }
    per_u = eng_peer_new(eng, pok_u->her_u);
  }

  if ( c3n == per_u->kno_o ) {
    if ( c3__pawn == u3_ship_rank(per_u->who_u) ) {
      eng_comet_proof(eng, per_u, lan_u);
    }
    else {
      eng_peer_ask(eng, per_u);
    }
    eng_peer_stash(eng, per_u, lan_u);   //  or they'll resend
    return;
  }

  ack_c = c3_malloc(ack_u->pat_s + 1);
  memcpy(ack_c, ack_u->pat_c, ack_u->pat_s);
  ack_c[ack_u->pat_s] = 0;
  pok_c = c3_malloc(pok_u->pat_s + 1);
  memcpy(pok_c, pok_u->pat_c, pok_u->pat_s);
  pok_c[pok_u->pat_s] = 0;

  //  both paths are two-party, between us
  //
  if (  (0 != strncmp(ack_c, "chum/", 5))
     || (0 != strncmp(pok_c, "chum/", 5))
     || !(aci_c = eng_open_chum(eng, per_u, ack_c))
     || !(poi_c = eng_open_chum(eng, per_u, pok_c))
     || (c3n == _flow_pith(aci_c, &ack_p))
     || (c3n == _flow_pith(poi_c, &pok_p)) )
  {
    goto done;
  }

  if (  (ack_u->rif_h != (c3_h)eng->rif_d)         //  our ack, this rift
     || (c3n == u3_ships_equal(pok_p.rcv_u, eng->our_u))
     || (pok_u->rif_h != (c3_h)per_u->rif_d)       //  their poke, their rift
     || (c3n == u3_ships_equal(ack_p.rcv_u, per_u->who_u))
     || (ack_p.bon_d != pok_p.bon_d)               //  the same flow
     || (ack_p.seq_d != pok_p.seq_d)
     || (ack_p.dir_e == pok_p.dir_e)
     || (0 != strcmp(ack_p.lod_c, "ack"))
     || (0 != strcmp(pok_p.lod_c, "poke")) )
  {
    goto done;
  }

  eng_peer_heard(eng, per_u, lan_u, pac_u->hed_u.hop_y, 0);
  eng_wait_add(eng, ack_c, lan_u);

  tof_d = mesa_num_leaves(dat_u->tob_d);

  if ( (AUTH_SIGN != dat_u->aut_u.typ_e) && (AUTH_HMAC != dat_u->aut_u.typ_e) ) {
    goto done;
  }

  if ( tof_d > 1 ) {
    //  the proof came along; authenticate it, then +peek the rest
    //
    c3_d      pon_d = lss_proof_size(tof_d);
    lss_hash* pof_u;

    if ( (dat_u->len_h != pon_d * sizeof(lss_hash)) || (dat_u->tob_d > ENG_MAX_MSG) ) {
      goto done;
    }
    pof_u = c3_malloc(pon_d * sizeof(lss_hash));
    memcpy(pof_u, dat_u->fra_y, pon_d * sizeof(lss_hash));
    lss_recover_root(roo_y, pof_u, (c3_h)pon_d);
    c3_free(pof_u);

    if ( c3y == eng_auth_page(eng, per_u, pok_c, roo_y, &dat_u->aut_u) ) {
      _peek_poke(eng, per_u, ack_p.bon_d, ack_p.dir_e, ack_p.seq_d, pok_c);
    }
    goto done;
  }

  if ( dat_u->tob_d != dat_u->len_h ) {
    //  one fragment, over the MTU with its names: we got the root
    //
    if ( 32 != dat_u->len_h ) goto done;
    memcpy(roo_y, dat_u->fra_y, 32);
    if ( c3y == eng_auth_page(eng, per_u, pok_c, roo_y, &dat_u->aut_u) ) {
      _peek_poke(eng, per_u, ack_p.bon_d, ack_p.dir_e, ack_p.seq_d, pok_c);
    }
    goto done;
  }

  {
    lss_hash  pof_u[1];
    c3_y*     pla_y;
    c3_z      pla_z;
    mesa_noun gag;

    mesa_blake3(dat_u->len_h, dat_u->fra_y, 32, pof_u[0]);
    if ( c3n == eng_auth_page(eng, per_u, pok_c, pof_u[0], &dat_u->aut_u) ) {
      mesa_log("mesa: %s: poke failed authentication",
               eng_ship_text(per_u->who_u));
      goto done;
    }
    if ( !(pla_y = eng_open_page(eng, per_u, pok_c, dat_u->len_h,
                                 dat_u->fra_y, &pla_z)) )
    {
      goto done;
    }
    gag = mesa_cue_bytes(pla_z, pla_y);
    c3_free(pla_y);
    if ( MESA_NONE != gag ) {
      eng_flow_sink(eng, per_u, ack_p.bon_d, ack_p.dir_e, ack_p.seq_d, gag);
      mesa_unref(gag);
    }
  }

done:
  c3_free(ack_c);
  c3_free(pok_c);
  c3_free(aci_c);
  c3_free(poi_c);
}

/* responses to our requests: +fo-take-ack, +fo-take-nax, +fo-take-cor
*/
static c3_o
_gage_is(mesa_noun gag, const c3_c* tag_c)
{
  char* hed_c;
  char* lod_c;
  c3_o  ret_o;

  if (  !mesa_is_cell(gag) || !mesa_is_atom(mesa_head(gag))
     || !mesa_is_cell(mesa_tail(gag)) || !mesa_is_atom(mesa_at(6, gag)) )
  {
    return c3n;
  }
  hed_c = mesa_atom_get_cord(mesa_head(gag));
  lod_c = mesa_atom_get_cord(mesa_at(6, gag));
  ret_o = __( !strcmp(hed_c, "message") && !strcmp(lod_c, tag_c) );
  c3_free(hed_c);
  c3_free(lod_c);
  return ret_o;
}

/* _pop(): remove our oldest message (acked or naxplained)
*/
static mesa_noun
_pop(mesa_engine* eng, eng_peer* per_u, eng_flow* flo_u)
{
  eng_msg*  msg_u = flo_u->lod_u;
  mesa_noun msg   = msg_u->msg;
  c3_y      key_y[40];

  if ( !(flo_u->lod_u = msg_u->nex_u) ) flo_u->lot_u = 0;
  eng_del(eng, key_y, eng_key(KEY_LOAD, per_u->who_u, flo_u->bon_d,
                              flo_u->dir_e, msg_u->seq_d, key_y));
  c3_free(msg_u);

  flo_u->win_w = 1;
  return msg;
}

static void
_give_done(mesa_engine* eng, eng_peer* per_u, c3_d bon_d, mesa_noun err)
{
  //  our ping flow is our own business
  //
  if ( c3y == _is_ping(eng, per_u, bon_d) ) {
    eng->pif_o = c3n;
    eng->pin_d = eng->now_d + ENG_PING_MS;
    return;
  }
  if ( c3y == eng_sys_done(eng, per_u, bon_d, err) ) {
    return;
  }
  if ( eng->hok_u.done ) {
    eng->hok_o = c3y;
    eng->hok_u.done(eng->hok_u.ctx, _her(per_u), bon_d, err);
    eng->hok_o = c3n;
  }
}

static void
_take_ack(mesa_engine* eng, eng_peer* per_u, eng_flow* flo_u, c3_d seq_d,
          mesa_noun gag)
{
  c3_d      nak_d;
  mesa_noun msg;
  c3_o      cok_o;
  c3_d      bon_d = flo_u->bon_d;
  eng_dire  dir_e = flo_u->dir_e;

  if (  !flo_u->lod_u
     || (seq_d >= flo_u->nex_d)
     || (flo_u->lod_u->seq_d != seq_d)
     || !mesa_atom_get_u64(mesa_tail(mesa_tail(gag)), &nak_d) )
  {
    return;
  }

  //  [%ack error=?]: %.y (0) is a nack
  //
  if ( 0 == nak_d ) {
    _peek_naxp(eng, per_u, flo_u, seq_d);
    return;
  }

  msg = _pop(eng, per_u, flo_u);

  //  the ack for our %cork: the flow is done
  //
  cok_o = __(  (DIRE_FOR == dir_e)
            && (c3y == flo_u->clo_o)
            && !flo_u->lod_u
            && (c3y == _is_cork_plea(msg)) );
  mesa_unref(msg);

  eng_flow_save(eng, per_u, flo_u);

  if ( c3y == cok_o ) {
    _flow_abel(eng, per_u, flo_u);
    if ( eng->hok_u.corked ) {
      eng->hok_o = c3y;
      eng->hok_u.corked(eng->hok_u.ctx, _her(per_u), bon_d);
      eng->hok_o = c3n;
    }
    return;
  }

  if ( c3n == flo_u->hat_o ) {
    eng_flow_send(eng, per_u, flo_u);
  }
  if ( DIRE_FOR == dir_e ) {
    _give_done(eng, per_u, bon_d, MESA_NONE);
  }
}

static void
_take_nax(mesa_engine* eng, eng_peer* per_u, eng_flow* flo_u, c3_d seq_d,
          mesa_noun gag)
{
  mesa_noun msg;
  c3_d      bon_d = flo_u->bon_d;
  c3_o      clo_o = flo_u->clo_o;

  if ( !flo_u->lod_u || (flo_u->lod_u->seq_d != seq_d) ) {
    return;
  }

  msg = _pop(eng, per_u, flo_u);
  mesa_unref(msg);
  eng_flow_save(eng, per_u, flo_u);

  if ( c3n == flo_u->hat_o ) {
    eng_flow_send(eng, per_u, flo_u);
  }
  if ( (c3n == clo_o) && (DIRE_FOR == flo_u->dir_e) ) {
    _give_done(eng, per_u, bon_d, mesa_tail(mesa_tail(gag)));
  }
}

/* eng_flow_take(): +ev-take-sage for a flow request
*/
void
eng_flow_take(mesa_engine* eng, eng_peer* per_u, eng_req* req_u,
              mesa_noun gag)
{
  eng_flow* flo_u = eng_flow_get(per_u, req_u->bon_d, req_u->dir_e);

  switch ( req_u->typ_e ) {
    case REQ_POKE: {
      eng_flow_sink(eng, per_u, req_u->bon_d, req_u->dir_e, req_u->seq_d, gag);
    } break;

    case REQ_ACK: {
      if ( flo_u && (c3y == _gage_is(gag, "ack")) ) {
        _take_ack(eng, per_u, flo_u, req_u->seq_d, gag);
      }
    } break;

    case REQ_NAXP: {
      if ( flo_u && (c3y == _gage_is(gag, "nax")) ) {
        _take_nax(eng, per_u, flo_u, req_u->seq_d, gag);
      }
    } break;

    case REQ_CORK: {
      //  they've removed their forward flow; remove our backward one
      //
      if (  flo_u
         && (c3y == _gage_is(gag, "gone"))
         && (c3y == flo_u->clo_o)
         && (c3n == flo_u->pen_o) )
      {
        _flow_abel(eng, per_u, flo_u);
      }
    } break;

    default: break;
  }
}

/* eng_flow_halt(): +pe-halt; their agent isn't running, so stop
**                  resending to it until they say otherwise (%spur)
*/
void
eng_flow_halt(mesa_engine* eng, eng_peer* per_u, eng_flow* flo_u)
{
  eng_req* req_u = per_u->req_u;

  if ( c3y == flo_u->hat_o ) {
    return;
  }
  flo_u->hat_o = c3y;
  eng_flow_save(eng, per_u, flo_u);

  while ( req_u ) {
    eng_req* nex_u = req_u->nex_u;
    if (  (REQ_ACK == req_u->typ_e)
       && (req_u->bon_d == flo_u->bon_d) && (req_u->dir_e == flo_u->dir_e) )
    {
      eng_req_del(per_u, req_u);
      eng_req_free(req_u);
    }
    req_u = nex_u;
  }
}

/* eng_flow_unhalt(): +ev-goad-flow; send again
*/
void
eng_flow_unhalt(mesa_engine* eng, eng_peer* per_u, eng_flow* flo_u)
{
  if ( c3n == flo_u->hat_o ) {
    return;
  }
  flo_u->hat_o = c3n;
  eng_flow_save(eng, per_u, flo_u);

  if ( flo_u->lod_u && (c3y == per_u->kno_o) ) {
    if ( 0 == flo_u->win_w ) {
      _flow_poke(eng, per_u, flo_u);
    }
    else {
      eng_flow_send(eng, per_u, flo_u);
    }
  }
}

/* eng_flow_resume(): restart requests after boot or once keys arrive
*/
void
eng_flow_resume(mesa_engine* eng, eng_peer* per_u)
{
  if ( c3n == per_u->kno_o ) {
    return;
  }

  for ( eng_flow_map_itr itr_u = eng_flow_map_first(&per_u->flo_u);
        !eng_flow_map_is_end(itr_u);
        itr_u = eng_flow_map_next(itr_u) )
  {
    eng_flow* flo_u = itr_u.data->val;

    if ( flo_u->lod_u && (c3n == flo_u->hat_o) ) {
      if ( 0 == flo_u->win_w ) {
        _flow_poke(eng, per_u, flo_u);       //  already sent; poke again
      }
      else {
        eng_flow_send(eng, per_u, flo_u);
      }
    }
    if ( (DIRE_BAK == flo_u->dir_e) && (c3y == flo_u->clo_o) ) {
      _peek_cork(eng, per_u, flo_u);
    }
  }
}

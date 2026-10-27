/// @file
///
/// Serving our namespace: +peek-mess and the publisher-side arms of |na
/// (+peek-publ, +peek-chum, +peek-flow, +peek-pawn) in ames.hoon, plus
/// vere's jumbo-frame cache and pending-interest table.

#include "eng.h"
#include "urcrypt.h"

/* cache
*/
static void
_line_free(eng_line* lin_u)
{
  c3_free(lin_u->pat_c);
  c3_free(lin_u->ser_y);
  c3_free(lin_u->pof_u);
  c3_free(lin_u->par_u);
  c3_free(lin_u->pap_y);
  c3_free(lin_u);
}

void
eng_cache_free(mesa_engine* eng)
{
  while ( eng->lin_u ) {
    eng_line* nex_u = eng->lin_u->nex_u;
    _line_free(eng->lin_u);
    eng->lin_u = nex_u;
  }
  eng->lit_u = 0;
  eng->lis_d = 0;
  eng_line_map_clear(&eng->lim_u);

  while ( eng->wat_u ) {
    eng_wait* nex_u = eng->wat_u->nex_u;
    c3_free(eng->wat_u->pat_c);
    c3_free(eng->wat_u);
    eng->wat_u = nex_u;
  }
}

static void
_cache_cut(mesa_engine* eng, eng_line* del_u)
{
  eng_line_map_erase(&eng->lim_u, del_u->pat_c);
  if ( del_u->pre_u ) del_u->pre_u->nex_u = del_u->nex_u;
  else                eng->lin_u = del_u->nex_u;
  if ( del_u->nex_u ) del_u->nex_u->pre_u = del_u->pre_u;
  else                eng->lit_u = del_u->pre_u;
  eng->lis_d -= del_u->tob_d;
  _line_free(del_u);
}

void
eng_cache_drop(mesa_engine* eng, const c3_c* pat_c)
{
  eng_line_map_itr itr_u = eng_line_map_get(&eng->lim_u, (c3_c*)pat_c);

  if ( !eng_line_map_is_end(itr_u) ) {
    _cache_cut(eng, itr_u.data->val);
  }
}

static void
_cache_put(mesa_engine* eng, eng_line* lin_u)
{
  //  evict oldest first
  //
  while ( eng->lin_u && (eng->lis_d + lin_u->tob_d > ENG_CACHE_MAX) ) {
    _cache_cut(eng, eng->lin_u);
  }

  lin_u->nex_u = 0;
  lin_u->pre_u = eng->lit_u;
  if ( eng->lit_u ) eng->lit_u->nex_u = lin_u;
  else              eng->lin_u = lin_u;
  eng->lit_u = lin_u;
  eng->lis_d += lin_u->tob_d;
  if ( eng_line_map_is_end(eng_line_map_insert(&eng->lim_u, lin_u->pat_c, lin_u)) ) {
    mesa_fail(__FILE__, __LINE__, "out of memory");
  }
}

/* _line_build(): fragment and hash a message
*/
static eng_line*
_line_build(c3_c* pat_c, c3_z tob_z, c3_y* ser_y)
{
  eng_line* lin_u = c3_calloc(sizeof(*lin_u));

  lin_u->pat_c = pat_c;
  lin_u->tob_d = tob_z;
  lin_u->ser_y = ser_y;
  lin_u->tof_d = mesa_num_leaves(tob_z);
  lin_u->pon_d = lss_proof_size(lin_u->tof_d);
  lin_u->pof_u = c3_calloc(lin_u->pon_d * sizeof(lss_hash));
  lin_u->par_u = c3_calloc(lin_u->tof_d * sizeof(lss_pair));
  lin_u->pap_y = c3_calloc(lin_u->tof_d);

  if ( 1 == lin_u->tof_d ) {
    lss_complete_inline_proof(lin_u->pof_u, ser_y, tob_z);
  }
  else {
    lss_builder* bil_u = c3_malloc(sizeof(*bil_u));
    lss_hash*    pof_u;

    lss_builder_init(bil_u, lin_u->tof_d);
    for ( c3_d i = 0; i < lin_u->tof_d; i++ ) {
      c3_z len_z = c3_min(tob_z - (i * ENG_FRAG), ENG_FRAG);
      lss_builder_ingest(bil_u, ser_y + (i * ENG_FRAG), len_z);
    }
    pof_u = lss_builder_finalize(bil_u);
    memcpy(lin_u->pof_u, pof_u, lin_u->pon_d * sizeof(lss_hash));

    for ( c3_d i = 0; i < lin_u->tof_d; i++ ) {
      lss_pair* par_u = lss_builder_pair(bil_u, i);
      if ( par_u ) {
        memcpy(lin_u->par_u[i], *par_u, sizeof(lss_pair));
        lin_u->pap_y[i] = 1;
      }
    }
    lss_builder_free(bil_u);
  }
  return lin_u;
}

/* _line_root(): what a page's authenticator signs: the message's BLAKE3
**               hash (+root:lss)
*/
static void
_line_root(eng_line* lin_u, c3_y roo_y[32])
{
  mesa_blake3(lin_u->tob_d, lin_u->ser_y, 32, roo_y);
}

/* _gage_message(): [%message page]
*/
static mesa_noun
_gage_message(mesa_noun pag)
{
  return mesa_cell(mesa_atom_cord("message"), pag);
}

/* _peek_pawn(): +peek-pawn, a comet's self-attestation
**
**   /pawn/proof/[rcvr]/[life]: [%message %proof (jam [sig msg])],
**   msg = (jam [pass sndr sndr-life rcvr rcvr-life])
*/
static mesa_noun
_peek_pawn(mesa_engine* eng, knot_path* pax_u, c3_w sat_w)
{
  u3_ship   rcv_u;
  c3_d      lyf_d;
  c3_y      who_y[16], rcv_y[16];
  mesa_noun opa, sig;
  c3_y*     msg_y;
  c3_z      msg_z;
  c3_y      sig_y[64];
  mesa_noun pro;

  if ( c3__pawn != u3_ship_rank(eng->our_u) ) {
    return MESA_NONE;
  }
  if (  (pax_u->len_w != sat_w + 4)
     || (c3n == knot_seg_is(&pax_u->seg_u[sat_w], "pawn"))
     || (c3n == knot_seg_is(&pax_u->seg_u[sat_w + 1], "proof"))
     || (c3n == knot_p_parse(pax_u->seg_u[sat_w + 2].len_z,
                             pax_u->seg_u[sat_w + 2].str_c, rcv_u))
     || (c3n == knot_ud_parse(pax_u->seg_u[sat_w + 3].len_z,
                              pax_u->seg_u[sat_w + 3].str_c, &lyf_d)) )
  {
    return MESA_NONE;
  }

  u3_ship_to_bytes(eng->our_u, 16, who_y);
  u3_ship_to_bytes(rcv_u, 16, rcv_y);

  opa = mesa_qual(mesa_atom_bytes(65, eng->pas_y),
                  mesa_atom_bytes(16, who_y),
                  mesa_atom_u64(eng->lyf_d),
                  mesa_cell(mesa_atom_bytes(16, rcv_y),
                            mesa_atom_u64(lyf_d)));
  msg_y = mesa_jam_bytes(opa, &msg_z);
  mesa_unref(opa);

  //  +sign-raw:ed over the jam's bytes
  //
  urcrypt_ed_sign_raw(msg_y, msg_z, eng->key_u.sgn_pub_y,
                      eng->key_u.sgn_sek_y, sig_y);
  sig = mesa_atom_bytes(64, sig_y);

  pro = mesa_cell(sig, mesa_atom_bytes(msg_z, msg_y));
  c3_free(msg_y);

  {
    mesa_noun jam = mesa_jam(pro);
    mesa_unref(pro);
    return _gage_message(mesa_cell(mesa_atom_cord("proof"), jam));
  }
}

/* _inner_text(): render segments [sat_w..] of a split path as "/a/b"
*/
static c3_c*
_inner_text(knot_path* pax_u, c3_w sat_w)
{
  c3_z  len_z = 1;
  c3_c* out_c;
  c3_z  o_z = 0;

  for ( c3_w i = sat_w; i < pax_u->len_w; i++ ) {
    len_z += 1 + pax_u->seg_u[i].len_z;
  }
  out_c = c3_malloc(len_z + 1);
  for ( c3_w i = sat_w; i < pax_u->len_w; i++ ) {
    out_c[o_z++] = '/';
    memcpy(out_c + o_z, pax_u->seg_u[i].str_c, pax_u->seg_u[i].len_z);
    o_z += pax_u->seg_u[i].len_z;
  }
  if ( !o_z ) {
    out_c[o_z++] = '/';
  }
  out_c[o_z] = 0;
  return out_c;
}

/* _flow_page(): +peek-flow on an inner "/a/x/1//flow/..." path read by
**               [per_u]
*/
static mesa_noun
_flow_page(mesa_engine* eng, eng_peer* per_u, const c3_c* inn_c)
{
  knot_path pax_u;
  c3_d      bon_d, seq_d = 0;
  u3_ship   rcv_u;
  eng_dire  dir_e;
  knot_seg* seg_u;
  mesa_noun pag;
  c3_c      lod_c[8];

  if ( c3n == knot_path_split(strlen(inn_c), inn_c, &pax_u) ) {
    return MESA_NONE;
  }
  seg_u = pax_u.seg_u;

  //  /a/x/1//flow/[bone]/[load]/[dire]/[rcvr](/[seq])
  //
  if (  (pax_u.len_w < 9)
     || (c3n == knot_seg_is(&seg_u[0], "a"))
     || (c3n == knot_seg_is(&seg_u[1], "x"))
     || (c3n == knot_seg_is(&seg_u[2], "1"))
     || (0 != seg_u[3].len_z)
     || (c3n == knot_seg_is(&seg_u[4], "flow"))
     || (c3n == knot_ud_parse(seg_u[5].len_z, seg_u[5].str_c, &bon_d))
     || (seg_u[6].len_z >= sizeof(lod_c)) )
  {
    return MESA_NONE;
  }

  memcpy(lod_c, seg_u[6].str_c, seg_u[6].len_z);
  lod_c[seg_u[6].len_z] = 0;

  if      ( c3y == knot_seg_is(&seg_u[7], "for") ) dir_e = DIRE_FOR;
  else if ( c3y == knot_seg_is(&seg_u[7], "bak") ) dir_e = DIRE_BAK;
  else return MESA_NONE;

  if ( c3n == knot_p_parse(seg_u[8].len_z, seg_u[8].str_c, rcv_u) ) {
    return MESA_NONE;
  }

  //  only the receiver named in the path may read it
  //
  if ( c3n == u3_ships_equal(rcv_u, per_u->who_u) ) {
    return MESA_NONE;
  }

  if ( 0 == strcmp(lod_c, "cork") ) {
    if ( 9 != pax_u.len_w ) return MESA_NONE;
  }
  else if (  (10 != pax_u.len_w)
          || (c3n == knot_ud_parse(seg_u[9].len_z, seg_u[9].str_c, &seq_d)) )
  {
    return MESA_NONE;
  }

  pag = eng_flow_peek(eng, per_u, bon_d, dir_e, lod_c, seq_d);
  return ( MESA_NONE == pag ) ? MESA_NONE : _gage_message(pag);
}

/* _line_make(): produce the message bound at [pat_c] in our namespace
*/
static eng_line*
_line_make(mesa_engine* eng, const c3_c* pat_c)
{
  knot_path pax_u;
  c3_c*     inn_c = 0;
  mesa_noun gag   = MESA_NONE;
  c3_y*     ser_y;
  c3_z      ser_z;
  eng_line* lin_u;
  eng_peer* per_u = 0;
  c3_y      cyf_y[16 + 1 + 1024];
  c3_z      cyf_z = 0;
  const c3_y* enk_y = 0;     //  encryption key: chum's or a shut key

  if ( c3n == knot_path_split(strlen(pat_c), pat_c, &pax_u) || !pax_u.len_w ) {
    return 0;
  }

  if ( c3y == knot_seg_is(&pax_u.seg_u[0], "publ") ) {
    c3_d lyf_d;

    if (  (pax_u.len_w < 2)
       || (c3n == knot_ud_parse(pax_u.seg_u[1].len_z,
                                pax_u.seg_u[1].str_c, &lyf_d))
       || (lyf_d != eng->lyf_d) )
    {
      return 0;
    }

    inn_c = _inner_text(&pax_u, 2);

    if ( 0 == strncmp(inn_c, "/a/x/1//pawn/proof/", 19) ) {
      gag = _peek_pawn(eng, &pax_u, 6);
    }
    else if ( 0 == strncmp(inn_c, "/a/", 3) ) {
      gag = MESA_NONE;   //  our own vane's state is never public
    }
    else if ( c3y == eng_shut_covered(eng, inn_c) ) {
      gag = MESA_NONE;   //  only with the key
    }
    else if ( eng->hok_u.peek ) {
      gag = eng->hok_u.peek(eng->hok_u.ctx, 0, inn_c);
    }
  }
  else if ( c3y == knot_seg_is(&pax_u.seg_u[0], "shut") ) {
    //  +peek-shut: anyone with the key may read what it covers
    //
    c3_d      kid_d;
    c3_y*     buf_y = eng_shut_parse(pat_c, &kid_d, &cyf_z);
    eng_shut* shu_u = buf_y ? eng_shut_get(eng, kid_d) : 0;

    if ( !shu_u || (cyf_z > sizeof(cyf_y)) ) {
      c3_free(buf_y);
      return 0;
    }
    memcpy(cyf_y, buf_y, cyf_z);
    c3_free(buf_y);

    inn_c = c3_malloc(cyf_z + 1);
    if (  (0 > mesa_open_path(shu_u->key_y, cyf_z, cyf_y, inn_c, cyf_z + 1))
       || (0 == strncmp(inn_c, "/a/", 3))
       || (c3n == eng_shut_under(shu_u->pre_c, inn_c)) )
    {
      c3_free(inn_c);
      return 0;
    }
    enk_y = shu_u->key_y;
    if ( eng->hok_u.peek ) {
      gag = eng->hok_u.peek(eng->hok_u.ctx, 0, inn_c);
    }
  }
  else if ( c3y == knot_seg_is(&pax_u.seg_u[0], "chum") ) {
    u3_ship who_u;

    if (  (5 != pax_u.len_w)
       || (c3n == knot_p_parse(pax_u.seg_u[2].len_z,
                               pax_u.seg_u[2].str_c, who_u))
       || !(per_u = eng_peer_get(eng, who_u))
       || (c3n == per_u->kno_o)
       || !(inn_c = eng_open_chum(eng, per_u, pat_c)) )
    {
      return 0;
    }

    //  the sealed path is the IV
    //
    {
      c3_y* buf_y = knot_uv_parse(pax_u.seg_u[4].len_z,
                                  pax_u.seg_u[4].str_c, &cyf_z);
      if ( !buf_y || (cyf_z > sizeof(cyf_y)) ) {
        c3_free(buf_y);
        c3_free(inn_c);
        return 0;
      }
      memcpy(cyf_y, buf_y, cyf_z);
      c3_free(buf_y);
    }
    enk_y = per_u->sym_y;

    if ( 0 == strncmp(inn_c, "/a/x/1//flow/", 13) ) {
      gag = _flow_page(eng, per_u, inn_c);
    }
    else if ( 0 == strncmp(inn_c, "/a/", 3) ) {
      gag = MESA_NONE;
    }
    else if ( eng->hok_u.peek ) {
      mesa_ship her = { .lo = per_u->who_u[0], .hi = per_u->who_u[1] };
      gag = eng->hok_u.peek(eng->hok_u.ctx, &her, inn_c);
    }
  }
  else {
    return 0;
  }

  c3_free(inn_c);

  if ( MESA_NONE == gag ) {
    return 0;
  }
  if ( !mesa_is_cell(gag) ) {
    mesa_unref(gag);
    return 0;
  }

  ser_y = mesa_jam_bytes(gag, &ser_z);
  mesa_unref(gag);

  if ( enk_y ) {
    //  +encrypt:crypt with the sealed path as IV
    //
    c3_y* cyr_y = c3_malloc(ser_z + 1);
    c3_z  cyr_z = mesa_encrypt(enk_y, cyf_z, cyf_y, ser_z, ser_y, cyr_y);
    c3_free(ser_y);
    ser_y = cyr_y;
    ser_z = cyr_z;
  }

  lin_u = _line_build(strdup(pat_c), ser_z, ser_y);

  //  authenticate the root: +sign (publ, shut) or +mac (chum)
  //
  {
    c3_y  roo_y[32];
    c3_c* pax_c = c3_malloc(strlen(pat_c) + 2);
    c3_c* ful_c;

    pax_c[0] = '/';
    strcpy(pax_c + 1, pat_c);
    ful_c = knot_beam(eng->our_u, pax_c);
    _line_root(lin_u, roo_y);

    if ( per_u ) {
      lin_u->aut_u.typ_e = AUTH_HMAC;
      mesa_mac_binding(per_u->sym_y, ful_c, roo_y, lin_u->aut_u.mac_y);
    }
    else {
      lin_u->aut_u.typ_e = AUTH_SIGN;
      mesa_sign_binding(&eng->key_u, ful_c, roo_y, lin_u->aut_u.sig_y);
    }
    c3_free(pax_c);
    c3_free(ful_c);
  }

  return lin_u;
}

eng_line*
eng_line_get(mesa_engine* eng, const c3_c* pat_c)
{
  eng_line*        lin_u;
  eng_line_map_itr itr_u = eng_line_map_get(&eng->lim_u, (c3_c*)pat_c);

  if ( !eng_line_map_is_end(itr_u) ) {
    return itr_u.data->val;
  }

  if ( !(lin_u = _line_make(eng, pat_c)) ) {
    return 0;
  }
  _cache_put(eng, lin_u);
  return lin_u;
}

/* eng_page_first(): the data for a first-contact (wan=~) page: the whole
**                   message if it fits in a fragment, else the proof.
**                   [*fre_y] is set to memory the caller frees.
*/
c3_o
eng_page_first(mesa_engine* eng, eng_line* lin_u, u3_mesa_data* dat_u,
               c3_y** fre_y)
{
  dat_u->tob_d = lin_u->tob_d;
  dat_u->aut_u = lin_u->aut_u;
  *fre_y = 0;

  if ( 1 == lin_u->tof_d ) {
    dat_u->len_h = lin_u->tob_d;
    dat_u->fra_y = lin_u->ser_y;
  }
  else {
    c3_y* pof_y = c3_malloc(lin_u->pon_d * sizeof(lss_hash));
    memcpy(pof_y, lin_u->pof_u, lin_u->pon_d * sizeof(lss_hash));
    dat_u->len_h = lin_u->pon_d * sizeof(lss_hash);
    dat_u->fra_y = pof_y;
    *fre_y = pof_y;
  }
  return c3y;
}

/* _serve(): answer a request for [nam_u] from [lin_u] to [lan_u]
*/
static void
_serve(mesa_engine* eng, eng_line* lin_u, u3_mesa_name* nam_u, mesa_lane lan_u)
{
  u3_mesa_pact  pac_u;
  u3_mesa_data* dat_u = &pac_u.pag_u.dat_u;
  c3_y*         fre_y = 0;

  memset(&pac_u, 0, sizeof(pac_u));
  pac_u.hed_u.typ_y = PACT_PAGE;
  pac_u.hed_u.nex_y = HOP_NONE;
  pac_u.pag_u.nam_u = *nam_u;
  pac_u.pag_u.nam_u.boq_y = ENG_BOQ;

  if ( c3y == nam_u->nit_o ) {
    eng_page_first(eng, lin_u, dat_u, &fre_y);
  }
  else if ( c3y == nam_u->aut_o ) {
    //  [%auth 0]: the proof (the root, for one fragment)
    //
    if ( 0 != nam_u->fra_d ) {
      return;
    }
    dat_u->tob_d = lin_u->tob_d;
    dat_u->aut_u = lin_u->aut_u;
    fre_y = c3_malloc(lin_u->pon_d * sizeof(lss_hash));
    if ( 1 == lin_u->tof_d ) {
      _line_root(lin_u, fre_y);
      dat_u->len_h = 32;
    }
    else {
      memcpy(fre_y, lin_u->pof_u, lin_u->pon_d * sizeof(lss_hash));
      dat_u->len_h = lin_u->pon_d * sizeof(lss_hash);
    }
    dat_u->fra_y = fre_y;
  }
  else {
    //  [%data fag]
    //
    c3_d fag_d = nam_u->fra_d;

    if ( fag_d >= lin_u->tof_d ) {
      return;
    }
    dat_u->tob_d = lin_u->tob_d;
    dat_u->fra_y = lin_u->ser_y + (fag_d * ENG_FRAG);
    dat_u->len_h = c3_min(lin_u->tob_d - (fag_d * ENG_FRAG), ENG_FRAG);

    if ( 1 == lin_u->tof_d ) {
      dat_u->aut_u = lin_u->aut_u;
    }
    else if ( lin_u->pap_y[fag_d] ) {
      dat_u->aut_u.typ_e = AUTH_PAIR;
      memcpy(dat_u->aut_u.has_y, lin_u->par_u[fag_d], sizeof(lss_pair));
    }
    else {
      dat_u->aut_u.typ_e = AUTH_NONE;
    }
  }

  eng_send_pact(eng, lan_u, &pac_u);
  c3_free(fre_y);
}

/* eng_serve_peek(): a %peek for something of ours
*/
void
eng_serve_peek(mesa_engine* eng, mesa_lane lan_u, u3_mesa_name* nam_u)
{
  c3_c*     pat_c;
  eng_line* lin_u;

  if ( (nam_u->rif_h != eng->rif_d) || (ENG_BOQ != nam_u->boq_y) ) {
    return;
  }

  pat_c = c3_malloc(nam_u->pat_s + 1);
  memcpy(pat_c, nam_u->pat_c, nam_u->pat_s);
  pat_c[nam_u->pat_s] = 0;

  if ( (lin_u = eng_line_get(eng, pat_c)) ) {
    _serve(eng, lin_u, nam_u, lan_u);
  }
  c3_free(pat_c);
}

/* responder pit: lanes that poked us, waiting for the ack page
*/
void
eng_wait_add(mesa_engine* eng, const c3_c* pat_c, mesa_lane lan_u)
{
  eng_wait** wat_u = &eng->wat_u;

  //  expire old entries while we're here
  //
  while ( *wat_u ) {
    eng_wait* cur_u = *wat_u;
    if ( cur_u->exp_d < eng->now_d ) {
      *wat_u = cur_u->nex_u;
      c3_free(cur_u->pat_c);
      c3_free(cur_u);
      continue;
    }
    if (  (0 == strcmp(cur_u->pat_c, pat_c))
       && (c3y == eng_lanes_eq(cur_u->lan_u, lan_u)) )
    {
      cur_u->exp_d = eng->now_d + ENG_PIT_MS;
      return;
    }
    wat_u = &cur_u->nex_u;
  }

  *wat_u = c3_calloc(sizeof(eng_wait));
  (*wat_u)->pat_c = strdup(pat_c);
  (*wat_u)->lan_u = lan_u;
  (*wat_u)->exp_d = eng->now_d + ENG_PIT_MS;
}

/* eng_give_page(): push a page of ours (an ack) to whoever is waiting
**                  for it, or else to [per_u]'s lanes
*/
void
eng_give_page(mesa_engine* eng, eng_peer* per_u, const c3_c* pat_c)
{
  eng_line*     lin_u = eng_line_get(eng, pat_c);
  u3_mesa_name  nam_u;
  eng_wait**    wat_u = &eng->wat_u;
  c3_o          sen_o = c3n;

  if ( !lin_u ) {
    return;
  }

  memset(&nam_u, 0, sizeof(nam_u));
  u3_ship_copy(nam_u.her_u, eng->our_u);
  nam_u.rif_h = eng->rif_d;
  nam_u.boq_y = ENG_BOQ;
  nam_u.nit_o = c3y;
  nam_u.aut_o = c3n;
  nam_u.pat_c = lin_u->pat_c;
  nam_u.pat_s = strlen(lin_u->pat_c);

  while ( *wat_u ) {
    eng_wait* cur_u = *wat_u;
    if ( 0 == strcmp(cur_u->pat_c, pat_c) ) {
      if ( cur_u->exp_d >= eng->now_d ) {
        _serve(eng, lin_u, &nam_u, cur_u->lan_u);
        sen_o = c3y;
      }
      *wat_u = cur_u->nex_u;
      c3_free(cur_u->pat_c);
      c3_free(cur_u);
      continue;
    }
    wat_u = &cur_u->nex_u;
  }

  if ( c3n == sen_o ) {
    mesa_lane las_u[2];
    c3_w      len_w = eng_peer_lanes(eng, per_u, las_u);

    for ( c3_w i = 0; i < len_w; i++ ) {
      _serve(eng, lin_u, &nam_u, las_u[i]);
    }
  }
}

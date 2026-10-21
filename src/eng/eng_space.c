/// @file
///
/// Namespace paths (+make-space-path, +decrypt-path) and page
/// authentication (+authenticate) / decryption (+decrypt-spac).
///
/// Namespace paths are kept without their leading '/', as packet names
/// carry them: "chum/1/~zod/1/0v1.abcde..." or "publ/1/g/x/...".

#include "eng.h"

/* eng_publ_path(): /publ/<life>/<inner>
*/
c3_c*
eng_publ_path(c3_d lyf_d, const c3_c* inn_c)
{
  c3_c  lyf_c[32];
  c3_z  len_z;
  c3_c* out_c;

  knot_ud(lyf_d, lyf_c);
  len_z = strlen(lyf_c) + strlen(inn_c) + 8;
  out_c = c3_malloc(len_z);
  snprintf(out_c, len_z, "publ/%s%s", lyf_c, inn_c);
  return out_c;
}

/* eng_chum_path(): a two-party path between us and [per_u]
**
**   [our_o] c3y: in our namespace, for [per_u] to read (+chum-to-her)
**           c3n: in their namespace, for us to read (+chum-to-our)
*/
c3_c*
eng_chum_path(mesa_engine* eng, eng_peer* per_u, c3_o our_o,
              const c3_c* inn_c)
{
  c3_y  sel_y[16 + 1 + 1024];
  c3_z  sel_z;
  c3_c  srv_c[32], cli_c[32];
  c3_c* cyf_c;
  c3_c* out_c;
  c3_z  len_z;
  c3_c* who_c;

  if ( strlen(inn_c) > 1024 ) {
    return 0;
  }

  sel_z = mesa_seal_path(per_u->sym_y, inn_c, sel_y);
  cyf_c = knot_uv(sel_z, sel_y);

  if ( c3y == our_o ) {
    knot_ud(eng->lyf_d, srv_c);
    knot_ud(per_u->lyf_d, cli_c);
    who_c = u3_ship_to_string(per_u->who_u);
  }
  else {
    knot_ud(per_u->lyf_d, srv_c);
    knot_ud(eng->lyf_d, cli_c);
    who_c = u3_ship_to_string(eng->our_u);
  }

  len_z = strlen(srv_c) + strlen(who_c) + strlen(cli_c) + strlen(cyf_c) + 16;
  out_c = c3_malloc(len_z);
  snprintf(out_c, len_z, "chum/%s/%s/%s/%s", srv_c, who_c, cli_c, cyf_c);

  c3_free(who_c);
  c3_free(cyf_c);
  return out_c;
}

/* _chum_parse(): check a chum path's lives and ship against [per_u];
**                produce the sealed path atom
*/
static c3_y*
_chum_parse(mesa_engine* eng, eng_peer* per_u, const c3_c* pat_c,
            c3_z* cyf_z)
{
  knot_path pax_u;
  c3_d      srv_d, cli_d;
  u3_ship   who_u;

  if (  (c3n == knot_path_split(strlen(pat_c), pat_c, &pax_u))
     || (5 != pax_u.len_w)
     || (c3n == knot_seg_is(&pax_u.seg_u[0], "chum"))
     || (c3n == knot_ud_parse(pax_u.seg_u[1].len_z, pax_u.seg_u[1].str_c, &srv_d))
     || (c3n == knot_p_parse(pax_u.seg_u[2].len_z, pax_u.seg_u[2].str_c, who_u))
     || (c3n == knot_ud_parse(pax_u.seg_u[3].len_z, pax_u.seg_u[3].str_c, &cli_d)) )
  {
    return 0;
  }

  if ( c3y == u3_ships_equal(who_u, per_u->who_u) ) {
    //  our namespace, read by them
    //
    if ( (srv_d != eng->lyf_d) || (cli_d != per_u->lyf_d) ) return 0;
  }
  else if ( c3y == u3_ships_equal(who_u, eng->our_u) ) {
    //  their namespace, read by us
    //
    if ( (srv_d != per_u->lyf_d) || (cli_d != eng->lyf_d) ) return 0;
  }
  else {
    return 0;
  }

  return knot_uv_parse(pax_u.seg_u[4].len_z, pax_u.seg_u[4].str_c, cyf_z);
}

/* eng_open_chum(): the inner path of a chum path ("/a/x/1//flow/...")
*/
c3_c*
eng_open_chum(mesa_engine* eng, eng_peer* per_u, const c3_c* pat_c)
{
  c3_z  cyf_z;
  c3_y* cyf_y = _chum_parse(eng, per_u, pat_c, &cyf_z);
  c3_c* inn_c;

  if ( !cyf_y ) {
    return 0;
  }

  inn_c = c3_malloc(cyf_z + 1);
  if ( 0 > mesa_open_path(per_u->sym_y, cyf_z, cyf_y, inn_c, cyf_z + 1) ) {
    c3_free(inn_c);
    inn_c = 0;
  }
  c3_free(cyf_y);
  return inn_c;
}

/* eng_auth_page(): +authenticate a page [per_u] bound at [pat_c]
*/
c3_o
eng_auth_page(mesa_engine* eng, eng_peer* per_u, const c3_c* pat_c,
              const c3_y roo_y[32], u3_auth_data* aut_u)
{
  c3_c* ful_c;
  c3_c* pax_c;
  c3_o  ret_o = c3n;
  c3_z  len_z = strlen(pat_c);

  pax_c = c3_malloc(len_z + 2);
  pax_c[0] = '/';
  memcpy(pax_c + 1, pat_c, len_z + 1);
  ful_c = knot_beam(per_u->who_u, pax_c);

  if ( (0 == strncmp(pat_c, "publ/", 5)) && (AUTH_SIGN == aut_u->typ_e) ) {
    knot_path pax_u;
    c3_d      lyf_d;

    if (  (c3y == knot_path_split(len_z, pat_c, &pax_u))
       && (2 <= pax_u.len_w)
       && (c3y == knot_ud_parse(pax_u.seg_u[1].len_z, pax_u.seg_u[1].str_c, &lyf_d))
       && (lyf_d == per_u->lyf_d) )
    {
      ret_o = mesa_verify_binding(per_u->sgn_y, aut_u->sig_y, ful_c, roo_y);
    }
  }
  else if ( (0 == strncmp(pat_c, "chum/", 5)) && (AUTH_HMAC == aut_u->typ_e) ) {
    c3_z  cyf_z;
    c3_y* cyf_y = _chum_parse(eng, per_u, pat_c, &cyf_z);

    if ( cyf_y ) {
      ret_o = mesa_verify_mac(per_u->sym_y, aut_u->mac_y, ful_c, roo_y);
      c3_free(cyf_y);
    }
  }

  c3_free(pax_c);
  c3_free(ful_c);
  return ret_o;
}

/* eng_open_page(): +decrypt-spac, a complete message's plaintext
*/
c3_y*
eng_open_page(mesa_engine* eng, eng_peer* per_u, const c3_c* pat_c,
              c3_z len_z, const c3_y* dat_y, c3_z* out_z)
{
  if ( 0 == strncmp(pat_c, "publ/", 5) ) {
    c3_y* out_y = c3_malloc(c3_max(len_z, 1));
    memcpy(out_y, dat_y, len_z);
    *out_z = len_z;
    return out_y;
  }

  if ( 0 == strncmp(pat_c, "chum/", 5) ) {
    c3_z  cyf_z;
    c3_y* cyf_y = _chum_parse(eng, per_u, pat_c, &cyf_z);
    c3_y* out_y;
    c3_ds res_ds;

    if ( !cyf_y ) {
      return 0;
    }
    out_y  = c3_malloc(c3_max(len_z, 1));
    res_ds = mesa_decrypt(per_u->sym_y, cyf_z, cyf_y, len_z, dat_y, out_y);
    c3_free(cyf_y);
    if ( res_ds < 0 ) {
      c3_free(out_y);
      return 0;
    }
    *out_z = (c3_z)res_ds;
    return out_y;
  }

  return 0;
}

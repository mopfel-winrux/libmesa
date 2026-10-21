/// @file

#include "compat.h"
#include "json.h"
#include "mesa/azimuth.h"

char*
mesa_azimuth_point_request(mesa_ship who)
{
  c3_c* who_c = mesa_ship_render(who);
  c3_c* out_c;
  c3_z  len_z = strlen(who_c) + 128;

  out_c = c3_malloc(len_z);
  snprintf(out_c, len_z,
           "{\"jsonrpc\":\"2.0\",\"id\":\"%s\",\"method\":\"getPoint\","
           "\"params\":{\"ship\":\"%s\"}}",
           who_c, who_c);

  c3_free(who_c);
  return out_c;
}

char*
mesa_azimuth_dns_request(void)
{
  return strdup("{\"jsonrpc\":\"2.0\",\"id\":\"turf\",\"method\":\"getDns\","
                "\"params\":{}}");
}

/* _hex32(): "0x..." (a big-endian atom) to 32 little-endian bytes, as
**           (cu (lead 32) (su ;~(pfix (jest '0x') hex))); c3n if it
**           isn't hex or won't fit.
*/
static c3_o
_hex32(const json* jon_u, c3_y out_y[32])
{
  const c3_c* hex_c = json_str(jon_u);
  c3_z        len_z;

  if ( !hex_c || ('0' != hex_c[0]) || ('x' != hex_c[1]) ) {
    return c3n;
  }
  hex_c += 2;
  len_z  = strlen(hex_c);

  if ( 0 == len_z ) {
    return c3n;
  }

  //  leading zeros don't count against the width
  //
  while ( (len_z > 1) && ('0' == hex_c[0]) ) {
    hex_c++;
    len_z--;
  }
  if ( len_z > 64 ) {
    return c3n;
  }

  memset(out_y, 0, 32);
  for ( c3_z i_z = 0; i_z < len_z; i_z++ ) {
    c3_c c = hex_c[len_z - 1 - i_z];
    c3_y v;

    if      ( (c >= '0') && (c <= '9') ) v = c - '0';
    else if ( (c >= 'a') && (c <= 'f') ) v = 10 + (c - 'a');
    else if ( (c >= 'A') && (c <= 'F') ) v = 10 + (c - 'A');
    else return c3n;

    out_y[i_z / 2] |= v << (4 * (i_z & 1));
  }
  return c3y;
}

mesa_azimuth_err
mesa_azimuth_point_parse(mesa_ship who,
                         size_t len,
                         const char* txt,
                         mesa_point* out)
{
  json*            jon_u = json_parse(len, txt);
  mesa_azimuth_err ret_e = MESA_AZIMUTH_BAD;
  json            *net_u, *key_u, *spo_u, *err_u;
  c3_d             sut_d, who_d;
  c3_y             aut_y[32], cry_y[32];

  if ( !jon_u ) {
    return MESA_AZIMUTH_BAD;
  }

  if ( (err_u = json_get(jon_u, "error")) ) {
    json* cod_u = json_get(err_u, "code");

    //  -32000 "Resource not found": the point hasn't been spawned
    //
    if (  cod_u && (JSON_NUM == cod_u->typ_e)
       && (0 == strcmp(cod_u->str_c, "-32000")) )
    {
      ret_e = MESA_AZIMUTH_NOT_FOUND;
    }
    goto done;
  }

  net_u = json_get(json_get(jon_u, "result"), "network");
  key_u = json_get(net_u, "keys");
  spo_u = json_get(net_u, "sponsor");

  memset(out, 0, sizeof(*out));
  out->who = who;

  if (  (c3n == json_u64(json_get(net_u, "rift"), &out->rift))
     || (c3n == json_u64(json_get(key_u, "life"), &out->life))
     || (c3n == json_u64(json_get(key_u, "suite"), &sut_d))
     || (c3n == _hex32(json_get(key_u, "auth"), aut_y))
     || (c3n == _hex32(json_get(key_u, "crypt"), cry_y))
     || !json_get(spo_u, "has")
     || (JSON_BOOL != json_get(spo_u, "has")->typ_e)
     || (c3n == json_u64(json_get(spo_u, "who"), &who_d)) )
  {
    goto done;
  }

  //  +pass-from-eth: unknown suites have no usable key
  //
  out->pass[0] = 'b';
  if ( 1 == sut_d ) {
    memcpy(out->pass + 1,  aut_y, 32);
    memcpy(out->pass + 33, cry_y, 32);
  }

  out->has_sponsor = ( c3y == json_get(spo_u, "has")->bol_o );
  out->sponsor     = (mesa_ship){ .lo = who_d, .hi = 0 };
  ret_e = MESA_AZIMUTH_OK;

done:
  json_free(jon_u);
  return ret_e;
}

int
mesa_azimuth_dns_parse(size_t len, const char* txt, char** out, size_t cap)
{
  json* jon_u = json_parse(len, txt);
  json* res_u = json_get(jon_u, "result");
  int   num_i = -1;

  if ( res_u && (JSON_ARR == res_u->typ_e) ) {
    c3_z i_z;

    for ( i_z = 0; i_z < res_u->len_z; i_z++ ) {
      if ( !json_str(res_u->val_u[i_z]) ) break;
    }

    if ( i_z == res_u->len_z ) {
      for ( i_z = 0; (i_z < res_u->len_z) && (i_z < cap); i_z++ ) {
        out[i_z] = strdup(res_u->val_u[i_z]->str_c);
      }
      num_i = (int)res_u->len_z;
    }
  }

  json_free(jon_u);
  return num_i;
}

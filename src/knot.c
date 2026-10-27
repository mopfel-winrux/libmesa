/// @file

#include "compat.h"
#include "knot.h"

c3_z
knot_ud(c3_d num_d, c3_c out_c[32])
{
  c3_c dig_c[20];
  c3_z n_z = 0, o_z = 0;

  do {
    dig_c[n_z++] = '0' + (num_d % 10);
    num_d /= 10;
  } while ( num_d );

  //  most significant first; '.' between groups of three
  //
  while ( n_z ) {
    out_c[o_z++] = dig_c[--n_z];
    if ( n_z && (0 == (n_z % 3)) ) {
      out_c[o_z++] = '.';
    }
  }
  out_c[o_z] = 0;
  return o_z;
}

c3_o
knot_ud_parse(c3_z len_z, const c3_c* txt_c, c3_d* out_d)
{
  c3_c can_c[32];
  c3_d val_d = 0;

  if ( !len_z || (len_z > 26) ) {
    return c3n;
  }

  for ( c3_z i_z = 0; i_z < len_z; i_z++ ) {
    c3_c c = txt_c[i_z];
    if ( '.' == c ) continue;
    if ( (c < '0') || (c > '9') ) return c3n;
    if ( val_d > (UINT64_MAX - 9) / 10 ) return c3n;
    val_d = (val_d * 10) + (c3_d)(c - '0');
  }

  //  canonical: re-render and compare (rejects "01", "1000", "1.00")
  //
  if ( (knot_ud(val_d, can_c) != len_z) || memcmp(can_c, txt_c, len_z) ) {
    return c3n;
  }
  *out_d = val_d;
  return c3y;
}

/* @uv: base32, groups of five digits, most significant first
*/
static const c3_c _uv_chars[] = "0123456789abcdefghijklmnopqrstuv";

static c3_ys
_uv_digit(c3_c c)
{
  if ( (c >= '0') && (c <= '9') ) return c - '0';
  if ( (c >= 'a') && (c <= 'v') ) return 10 + (c - 'a');
  return -1;
}

/* _bits_at(): [wid_w] (at most 8) bits at bit [bit_z], LSB first
*/
static c3_y
_bits_at(c3_z len_z, const c3_y* buf_y, c3_z bit_z, c3_w wid_w)
{
  c3_z i_z = bit_z >> 3;
  c3_w two_w;

  if ( i_z >= len_z ) return 0;
  two_w = buf_y[i_z] | ((i_z + 1 < len_z) ? (buf_y[i_z + 1] << 8) : 0);
  return (c3_y)((two_w >> (bit_z & 7)) & ((1u << wid_w) - 1));
}

c3_c*
knot_uv(c3_z len_z, const c3_y* buf_y)
{
  c3_z  dig_z;
  c3_c* out_c;
  c3_z  o_z = 2;

  while ( len_z && !buf_y[len_z - 1] ) len_z--;

  if ( !len_z ) {
    return strdup("0v0");
  }

  dig_z = ((len_z * 8) + 4) / 5;
  while ( dig_z && !_bits_at(len_z, buf_y, (dig_z - 1) * 5, 5) ) {
    dig_z--;
  }

  out_c = c3_malloc(3 + dig_z + (dig_z / 5));
  out_c[0] = '0';
  out_c[1] = 'v';

  for ( c3_z d = dig_z; d-- > 0; ) {
    out_c[o_z++] = _uv_chars[_bits_at(len_z, buf_y, d * 5, 5)];
    if ( d && (0 == (d % 5)) ) {
      out_c[o_z++] = '.';
    }
  }
  out_c[o_z] = 0;
  return out_c;
}

c3_y*
knot_uv_parse(c3_z len_z, const c3_c* txt_c, c3_z* out_z)
{
  c3_z  dig_z = 0;
  c3_z  cap_z;
  c3_y* buf_y;
  c3_z  bit_z = 0;
  c3_c* can_c;

  if ( (len_z < 3) || ('0' != txt_c[0]) || ('v' != txt_c[1]) ) {
    return 0;
  }

  for ( c3_z i_z = 2; i_z < len_z; i_z++ ) {
    if ( _uv_digit(txt_c[i_z]) >= 0 ) dig_z++;
    else if ( '.' != txt_c[i_z] ) return 0;
  }
  if ( !dig_z ) {
    return 0;
  }

  cap_z = ((dig_z * 5) + 7) / 8;
  buf_y = c3_calloc(cap_z);

  for ( c3_z i_z = len_z; i_z-- > 2; ) {
    c3_ys dig = _uv_digit(txt_c[i_z]);
    c3_w  two_w;

    if ( dig < 0 ) continue;
    two_w = (c3_w)dig << (bit_z & 7);
    buf_y[bit_z >> 3] |= (c3_y)two_w;
    if ( (two_w >> 8) && ((bit_z >> 3) + 1 < cap_z) ) {
      buf_y[(bit_z >> 3) + 1] |= (c3_y)(two_w >> 8);
    }
    bit_z += 5;
  }

  //  canonical form only
  //
  can_c = knot_uv(cap_z, buf_y);
  if ( (strlen(can_c) != len_z) || memcmp(can_c, txt_c, len_z) ) {
    c3_free(can_c);
    c3_free(buf_y);
    return 0;
  }
  c3_free(can_c);

  *out_z = cap_z;
  return buf_y;
}

c3_o
knot_p_parse(c3_z len_z, const c3_c* txt_c, u3_ship who_u)
{
  c3_c buf_c[64];

  if ( !len_z || (len_z >= sizeof(buf_c)) || ('~' != txt_c[0]) ) {
    return c3n;
  }
  memcpy(buf_c, txt_c, len_z);
  buf_c[len_z] = 0;
  return u3_ship_of_string(who_u, buf_c);
}

/* knots are @ta: lowercase, digits, '-', '~', '_', '.'
*/
static c3_o
_knot_char(c3_c c)
{
  return __( ((c >= 'a') && (c <= 'z'))
          || ((c >= '0') && (c <= '9'))
          || ('-' == c) || ('~' == c) || ('_' == c) || ('.' == c) );
}

c3_o
knot_path_split(c3_z len_z, const c3_c* pat_c, knot_path* out_u)
{
  c3_z i_z = 0;

  out_u->len_w = 0;

  if ( len_z && ('/' == pat_c[0]) ) {
    i_z = 1;
  }
  if ( i_z == len_z ) {
    return c3y;   //  "/" or "" is the empty path
  }

  while ( 1 ) {
    c3_z sat_z = i_z;

    while ( (i_z < len_z) && ('/' != pat_c[i_z]) ) {
      if ( c3n == _knot_char(pat_c[i_z]) ) {
        return c3n;
      }
      i_z++;
    }

    if ( out_u->len_w == PATH_MAX_SEGS ) {
      return c3n;
    }
    out_u->seg_u[out_u->len_w].str_c = pat_c + sat_z;
    out_u->seg_u[out_u->len_w].len_z = i_z - sat_z;
    out_u->len_w++;

    if ( i_z == len_z ) {
      return c3y;
    }
    i_z++;   //  '/'
  }
}

c3_o
knot_seg_is(const knot_seg* seg_u, const c3_c* str_c)
{
  c3_z len_z = strlen(str_c);
  return __( (len_z == seg_u->len_z) && !memcmp(seg_u->str_c, str_c, len_z) );
}

c3_c*
knot_beam(u3_ship who_u, const c3_c* pat_c)
{
  c3_c* who_c = u3_ship_to_string(who_u);
  c3_z  len_z = strlen(who_c) + strlen(pat_c) + 8;
  c3_c* out_c = c3_malloc(len_z);

  snprintf(out_c, len_z, "/%s//1%s", who_c, pat_c);
  c3_free(who_c);
  return out_c;
}

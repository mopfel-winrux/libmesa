/// @file
///
/// AES-256-SIV (RFC 5297), as Hoon's ++sivc:aes:crypto: classic ames'
/// packet cipher. Self-contained (an AES-256 block encryption, CMAC, S2V
/// and CTR), so libmesa needs no OpenSSL for it.
///
/// Hoon treats its atoms as big-endian byte strings here (vere's jet
/// reverses each one), so the wrappers below take little-endian atom bytes
/// and reverse them around the standard construction.

#include "compat.h"
#include "aes_siv.h"

#include <string.h>

/* AES-256 encryption (FIPS-197)
*/
static const c3_y _sbox[256] = {
  0x63,0x7c,0x77,0x7b,0xf2,0x6b,0x6f,0xc5,0x30,0x01,0x67,0x2b,0xfe,0xd7,0xab,0x76,
  0xca,0x82,0xc9,0x7d,0xfa,0x59,0x47,0xf0,0xad,0xd4,0xa2,0xaf,0x9c,0xa4,0x72,0xc0,
  0xb7,0xfd,0x93,0x26,0x36,0x3f,0xf7,0xcc,0x34,0xa5,0xe5,0xf1,0x71,0xd8,0x31,0x15,
  0x04,0xc7,0x23,0xc3,0x18,0x96,0x05,0x9a,0x07,0x12,0x80,0xe2,0xeb,0x27,0xb2,0x75,
  0x09,0x83,0x2c,0x1a,0x1b,0x6e,0x5a,0xa0,0x52,0x3b,0xd6,0xb3,0x29,0xe3,0x2f,0x84,
  0x53,0xd1,0x00,0xed,0x20,0xfc,0xb1,0x5b,0x6a,0xcb,0xbe,0x39,0x4a,0x4c,0x58,0xcf,
  0xd0,0xef,0xaa,0xfb,0x43,0x4d,0x33,0x85,0x45,0xf9,0x02,0x7f,0x50,0x3c,0x9f,0xa8,
  0x51,0xa3,0x40,0x8f,0x92,0x9d,0x38,0xf5,0xbc,0xb6,0xda,0x21,0x10,0xff,0xf3,0xd2,
  0xcd,0x0c,0x13,0xec,0x5f,0x97,0x44,0x17,0xc4,0xa7,0x7e,0x3d,0x64,0x5d,0x19,0x73,
  0x60,0x81,0x4f,0xdc,0x22,0x2a,0x90,0x88,0x46,0xee,0xb8,0x14,0xde,0x5e,0x0b,0xdb,
  0xe0,0x32,0x3a,0x0a,0x49,0x06,0x24,0x5c,0xc2,0xd3,0xac,0x62,0x91,0x95,0xe4,0x79,
  0xe7,0xc8,0x37,0x6d,0x8d,0xd5,0x4e,0xa9,0x6c,0x56,0xf4,0xea,0x65,0x7a,0xae,0x08,
  0xba,0x78,0x25,0x2e,0x1c,0xa6,0xb4,0xc6,0xe8,0xdd,0x74,0x1f,0x4b,0xbd,0x8b,0x8a,
  0x70,0x3e,0xb5,0x66,0x48,0x03,0xf6,0x0e,0x61,0x35,0x57,0xb9,0x86,0xc1,0x1d,0x9e,
  0xe1,0xf8,0x98,0x11,0x69,0xd9,0x8e,0x94,0x9b,0x1e,0x87,0xe9,0xce,0x55,0x28,0xdf,
  0x8c,0xa1,0x89,0x0d,0xbf,0xe6,0x42,0x68,0x41,0x99,0x2d,0x0f,0xb0,0x54,0xbb,0x16,
};

typedef struct {
  c3_y rk_y[15][16];      //  round keys
} _aes256;

static c3_y
_xtime(c3_y x_y)
{
  return (c3_y)((x_y << 1) ^ ((x_y & 0x80) ? 0x1b : 0));
}

static void
_aes_key(_aes256* aes_u, const c3_y key_y[32])
{
  c3_y w_y[240];
  c3_y rc_y = 1;

  memcpy(w_y, key_y, 32);
  for ( c3_w i = 32; i < 240; i += 4 ) {
    c3_y t_y[4];
    memcpy(t_y, w_y + i - 4, 4);
    if ( 0 == (i % 32) ) {
      c3_y f_y = t_y[0];
      t_y[0] = _sbox[t_y[1]] ^ rc_y;
      t_y[1] = _sbox[t_y[2]];
      t_y[2] = _sbox[t_y[3]];
      t_y[3] = _sbox[f_y];
      rc_y = _xtime(rc_y);
    }
    else if ( 16 == (i % 32) ) {
      for ( c3_w j = 0; j < 4; j++ ) t_y[j] = _sbox[t_y[j]];
    }
    for ( c3_w j = 0; j < 4; j++ ) {
      w_y[i + j] = w_y[i + j - 32] ^ t_y[j];
    }
  }
  memcpy(aes_u->rk_y, w_y, 240);
}

static void
_aes_block(const _aes256* aes_u, const c3_y in_y[16], c3_y out_y[16])
{
  c3_y s_y[16];

  for ( c3_w i = 0; i < 16; i++ ) s_y[i] = in_y[i] ^ aes_u->rk_y[0][i];

  for ( c3_w r = 1; r <= 14; r++ ) {
    c3_y t_y[16];
    //  sub-bytes and shift-rows (state is column-major)
    for ( c3_w c = 0; c < 4; c++ ) {
      for ( c3_w row = 0; row < 4; row++ ) {
        t_y[4 * c + row] = _sbox[s_y[4 * ((c + row) % 4) + row]];
      }
    }
    //  mix-columns, but not in the last round
    if ( r < 14 ) {
      for ( c3_w c = 0; c < 4; c++ ) {
        c3_y* col_y = t_y + 4 * c;
        c3_y  a0 = col_y[0], a1 = col_y[1], a2 = col_y[2], a3 = col_y[3];
        c3_y  all_y = a0 ^ a1 ^ a2 ^ a3;
        col_y[0] = a0 ^ all_y ^ _xtime(a0 ^ a1);
        col_y[1] = a1 ^ all_y ^ _xtime(a1 ^ a2);
        col_y[2] = a2 ^ all_y ^ _xtime(a2 ^ a3);
        col_y[3] = a3 ^ all_y ^ _xtime(a3 ^ a0);
      }
    }
    for ( c3_w i = 0; i < 16; i++ ) s_y[i] = t_y[i] ^ aes_u->rk_y[r][i];
  }
  memcpy(out_y, s_y, 16);
}

/* CMAC (RFC 4493) and S2V (RFC 5297)
*/
static void
_dbl(c3_y b_y[16])
{
  c3_y car_y = b_y[0] & 0x80;
  for ( c3_w i = 0; i < 15; i++ ) {
    b_y[i] = (c3_y)((b_y[i] << 1) | (b_y[i + 1] >> 7));
  }
  b_y[15] = (c3_y)(b_y[15] << 1);
  if ( car_y ) b_y[15] ^= 0x87;
}

static void
_cmac(const _aes256* aes_u, const c3_y* msg_y, c3_z len_z, c3_y out_y[16])
{
  c3_y k1_y[16] = {0}, k2_y[16], x_y[16] = {0}, blk_y[16];
  c3_z n_z = ( 0 == len_z ) ? 1 : (len_z + 15) / 16;
  c3_o ful_o = __( len_z && (0 == len_z % 16) );

  _aes_block(aes_u, k1_y, k1_y);      //  L = AES(0)
  _dbl(k1_y);                          //  K1
  memcpy(k2_y, k1_y, 16);
  _dbl(k2_y);                          //  K2

  for ( c3_z i = 0; i < n_z; i++ ) {
    if ( i + 1 < n_z ) {
      memcpy(blk_y, msg_y + 16 * i, 16);
    }
    else {
      c3_z las_z = len_z - 16 * i;
      memset(blk_y, 0, 16);
      memcpy(blk_y, msg_y + 16 * i, las_z);
      if ( c3y == ful_o ) {
        for ( c3_w j = 0; j < 16; j++ ) blk_y[j] ^= k1_y[j];
      }
      else {
        blk_y[las_z] = 0x80;
        for ( c3_w j = 0; j < 16; j++ ) blk_y[j] ^= k2_y[j];
      }
    }
    for ( c3_w j = 0; j < 16; j++ ) x_y[j] ^= blk_y[j];
    _aes_block(aes_u, x_y, x_y);
  }
  memcpy(out_y, x_y, 16);
}

static void
_s2v(const _aes256* aes_u, c3_w ads_w, const c3_y** ads_y, const c3_z* adz_z,
     const c3_y* msg_y, c3_z msg_z, c3_y v_y[16])
{
  c3_y d_y[16] = {0}, t_y[16];

  _cmac(aes_u, d_y, 16, d_y);
  for ( c3_w i = 0; i < ads_w; i++ ) {
    _dbl(d_y);
    _cmac(aes_u, ads_y[i], adz_z[i], t_y);
    for ( c3_w j = 0; j < 16; j++ ) d_y[j] ^= t_y[j];
  }

  if ( msg_z >= 16 ) {
    c3_y* buf_y = c3_malloc(msg_z);
    memcpy(buf_y, msg_y, msg_z);
    for ( c3_w j = 0; j < 16; j++ ) buf_y[msg_z - 16 + j] ^= d_y[j];
    _cmac(aes_u, buf_y, msg_z, v_y);
    c3_free(buf_y);
  }
  else {
    c3_y pad_y[16] = {0};
    _dbl(d_y);
    memcpy(pad_y, msg_y, msg_z);
    pad_y[msg_z] = 0x80;
    for ( c3_w j = 0; j < 16; j++ ) pad_y[j] ^= d_y[j];
    _cmac(aes_u, pad_y, 16, v_y);
  }
}

static void
_ctr(const _aes256* aes_u, const c3_y v_y[16], const c3_y* in_y, c3_z len_z,
     c3_y* out_y)
{
  c3_y q_y[16], ks_y[16];

  memcpy(q_y, v_y, 16);
  q_y[8]  &= 0x7f;
  q_y[12] &= 0x7f;

  for ( c3_z i = 0; i < len_z; i += 16 ) {
    _aes_block(aes_u, q_y, ks_y);
    for ( c3_z j = 0; (j < 16) && (i + j < len_z); j++ ) {
      out_y[i + j] = in_y[i + j] ^ ks_y[j];
    }
    for ( c3_w j = 16; j-- > 0; ) {
      if ( ++q_y[j] ) break;
    }
  }
}

/* the Hoon-compatible interface: little-endian atom bytes
*/
static void
_rev(const c3_y* in_y, c3_z len_z, c3_y* out_y)
{
  for ( c3_z i = 0; i < len_z; i++ ) out_y[i] = in_y[len_z - 1 - i];
}

static void
_keys(const c3_y key_y[64], _aes256* mac_u, _aes256* ctr_u)
{
  c3_y kbe_y[64];

  //  k1 (S2V) is the high half of the atom, k2 (CTR) the low
  //
  _rev(key_y, 64, kbe_y);
  _aes_key(mac_u, kbe_y);
  _aes_key(ctr_u, kbe_y + 32);
}

static void
_ads(c3_w ads_w, const c3_y** ads_y, const c3_z* adz_z, c3_y** rev_y)
{
  for ( c3_w i = 0; i < ads_w; i++ ) {
    rev_y[i] = c3_malloc(adz_z[i] ? adz_z[i] : 1);
    _rev(ads_y[i], adz_z[i], rev_y[i]);
  }
}

void
mesa_sivc_en(const c3_y key_y[64], c3_w ads_w, const c3_y** ads_y,
             const c3_z* adz_z, const c3_y* msg_y, c3_z msg_z,
             c3_y iv_y[16], c3_y* cyf_y)
{
  _aes256 mac_u, ctr_u;
  c3_y*   rev_y[8];
  c3_y*   mbe_y = c3_malloc(msg_z ? msg_z : 1);
  c3_y*   cbe_y = c3_malloc(msg_z ? msg_z : 1);
  c3_y    v_y[16];

  if ( ads_w > 8 ) ads_w = 8;
  _keys(key_y, &mac_u, &ctr_u);
  _ads(ads_w, ads_y, adz_z, rev_y);
  _rev(msg_y, msg_z, mbe_y);

  _s2v(&mac_u, ads_w, (const c3_y**)rev_y, adz_z, mbe_y, msg_z, v_y);
  _ctr(&ctr_u, v_y, mbe_y, msg_z, cbe_y);

  _rev(v_y, 16, iv_y);
  _rev(cbe_y, msg_z, cyf_y);

  for ( c3_w i = 0; i < ads_w; i++ ) c3_free(rev_y[i]);
  c3_free(mbe_y);
  c3_free(cbe_y);
}

c3_o
mesa_sivc_de(const c3_y key_y[64], c3_w ads_w, const c3_y** ads_y,
             const c3_z* adz_z, const c3_y iv_y[16], const c3_y* cyf_y,
             c3_z cyf_z, c3_y* msg_y)
{
  _aes256 mac_u, ctr_u;
  c3_y*   rev_y[8];
  c3_y*   cbe_y = c3_malloc(cyf_z ? cyf_z : 1);
  c3_y*   mbe_y = c3_malloc(cyf_z ? cyf_z : 1);
  c3_y    v_y[16], w_y[16];
  c3_y    dif_y = 0;

  if ( ads_w > 8 ) ads_w = 8;
  _keys(key_y, &mac_u, &ctr_u);
  _ads(ads_w, ads_y, adz_z, rev_y);
  _rev(iv_y, 16, v_y);
  _rev(cyf_y, cyf_z, cbe_y);

  _ctr(&ctr_u, v_y, cbe_y, cyf_z, mbe_y);
  _s2v(&mac_u, ads_w, (const c3_y**)rev_y, adz_z, mbe_y, cyf_z, w_y);
  for ( c3_w j = 0; j < 16; j++ ) dif_y |= (c3_y)(v_y[j] ^ w_y[j]);

  _rev(mbe_y, cyf_z, msg_y);
  for ( c3_w i = 0; i < ads_w; i++ ) c3_free(rev_y[i]);
  c3_free(mbe_y);
  c3_free(cbe_y);
  return __( 0 == dif_y );
}

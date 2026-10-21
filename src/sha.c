/// @file

#include "compat.h"
#include "crypto.h"
#include "sha.h"
#include "sha512.h"

/* sha-256 (FIPS 180-4)
*/
static const c3_h _sha256_k[64] = {
  0x428a2f98, 0x71374491, 0xb5c0fbcf, 0xe9b5dba5, 0x3956c25b, 0x59f111f1,
  0x923f82a4, 0xab1c5ed5, 0xd807aa98, 0x12835b01, 0x243185be, 0x550c7dc3,
  0x72be5d74, 0x80deb1fe, 0x9bdc06a7, 0xc19bf174, 0xe49b69c1, 0xefbe4786,
  0x0fc19dc6, 0x240ca1cc, 0x2de92c6f, 0x4a7484aa, 0x5cb0a9dc, 0x76f988da,
  0x983e5152, 0xa831c66d, 0xb00327c8, 0xbf597fc7, 0xc6e00bf3, 0xd5a79147,
  0x06ca6351, 0x14292967, 0x27b70a85, 0x2e1b2138, 0x4d2c6dfc, 0x53380d13,
  0x650a7354, 0x766a0abb, 0x81c2c92e, 0x92722c85, 0xa2bfe8a1, 0xa81a664b,
  0xc24b8b70, 0xc76c51a3, 0xd192e819, 0xd6990624, 0xf40e3585, 0x106aa070,
  0x19a4c116, 0x1e376c08, 0x2748774c, 0x34b0bcb5, 0x391c0cb3, 0x4ed8aa4a,
  0x5b9cca4f, 0x682e6ff3, 0x748f82ee, 0x78a5636f, 0x84c87814, 0x8cc70208,
  0x90befffa, 0xa4506ceb, 0xbef9a3f7, 0xc67178f2,
};

#define _ror(x, n) (((x) >> (n)) | ((x) << (32 - (n))))

static void
_sha256_block(c3_h has_w[8], const c3_y blk_y[64])
{
  c3_h w_w[64];
  c3_h a, b, c, d, e, f, g, h;

  for ( c3_h i = 0; i < 16; i++ ) {
    w_w[i] = ((c3_h)blk_y[4*i] << 24) | ((c3_h)blk_y[4*i + 1] << 16)
           | ((c3_h)blk_y[4*i + 2] << 8) | (c3_h)blk_y[4*i + 3];
  }
  for ( c3_h i = 16; i < 64; i++ ) {
    c3_h s0 = _ror(w_w[i-15], 7) ^ _ror(w_w[i-15], 18) ^ (w_w[i-15] >> 3);
    c3_h s1 = _ror(w_w[i-2], 17) ^ _ror(w_w[i-2], 19) ^ (w_w[i-2] >> 10);
    w_w[i] = w_w[i-16] + s0 + w_w[i-7] + s1;
  }

  a = has_w[0]; b = has_w[1]; c = has_w[2]; d = has_w[3];
  e = has_w[4]; f = has_w[5]; g = has_w[6]; h = has_w[7];

  for ( c3_h i = 0; i < 64; i++ ) {
    c3_h s1 = _ror(e, 6) ^ _ror(e, 11) ^ _ror(e, 25);
    c3_h ch = (e & f) ^ (~e & g);
    c3_h t1 = h + s1 + ch + _sha256_k[i] + w_w[i];
    c3_h s0 = _ror(a, 2) ^ _ror(a, 13) ^ _ror(a, 22);
    c3_h mj = (a & b) ^ (a & c) ^ (b & c);
    c3_h t2 = s0 + mj;

    h = g; g = f; f = e; e = d + t1;
    d = c; c = b; b = a; a = t1 + t2;
  }

  has_w[0] += a; has_w[1] += b; has_w[2] += c; has_w[3] += d;
  has_w[4] += e; has_w[5] += f; has_w[6] += g; has_w[7] += h;
}

void
mesa_shay(c3_z len_z, const c3_y* msg_y, c3_y out_y[32])
{
  c3_h has_w[8] = {
    0x6a09e667, 0xbb67ae85, 0x3c6ef372, 0xa54ff53a,
    0x510e527f, 0x9b05688c, 0x1f83d9ab, 0x5be0cd19,
  };
  c3_y blk_y[128] = {0};
  c3_z rem_z = len_z % 64;
  c3_z end_z = len_z - rem_z;
  c3_z pad_z = ( rem_z < 56 ) ? 64 : 128;
  c3_d bit_d = (c3_d)len_z * 8;

  for ( c3_z i_z = 0; i_z < end_z; i_z += 64 ) {
    _sha256_block(has_w, msg_y + i_z);
  }

  if ( rem_z ) {
    memcpy(blk_y, msg_y + end_z, rem_z);
  }
  blk_y[rem_z] = 0x80;
  for ( c3_h i = 0; i < 8; i++ ) {
    blk_y[pad_z - 1 - i] = (c3_y)(bit_d >> (8 * i));
  }
  _sha256_block(has_w, blk_y);
  if ( 128 == pad_z ) {
    _sha256_block(has_w, blk_y + 64);
  }

  for ( c3_h i = 0; i < 8; i++ ) {
    out_y[4*i]     = (c3_y)(has_w[i] >> 24);
    out_y[4*i + 1] = (c3_y)(has_w[i] >> 16);
    out_y[4*i + 2] = (c3_y)(has_w[i] >> 8);
    out_y[4*i + 3] = (c3_y)has_w[i];
  }
}

void
mesa_shax(c3_z len_z, const c3_y* msg_y, c3_y out_y[32])
{
  mesa_shay(mesa_met3_bytes(len_z, msg_y), msg_y, out_y);
}

void
mesa_shal(c3_z len_z, const c3_y* msg_y, c3_y out_y[64])
{
  sha512(msg_y, len_z, out_y);
}

void
mesa_shas(c3_z sal_z, const c3_y* sal_y,
          c3_z len_z, const c3_y* msg_y,
          c3_y out_y[32])
{
  c3_y  mid_y[32];
  c3_z  wid_z;
  c3_y* buf_y;

  sal_z = mesa_met3_bytes(sal_z, sal_y);
  wid_z = c3_max(32, sal_z);
  buf_y = c3_calloc(wid_z);

  mesa_shax(len_z, msg_y, mid_y);
  memcpy(buf_y, sal_y, sal_z);
  for ( c3_z i_z = 0; i_z < 32; i_z++ ) {
    buf_y[i_z] ^= mid_y[i_z];
  }

  mesa_shay(wid_z, buf_y, out_y);
  c3_free(buf_y);
}

void
mesa_shaf(c3_z sal_z, const c3_y* sal_y,
          c3_z len_z, const c3_y* msg_y,
          c3_y out_y[16])
{
  c3_y haz_y[32];

  mesa_shas(sal_z, sal_y, len_z, msg_y, haz_y);
  for ( c3_z i_z = 0; i_z < 16; i_z++ ) {
    out_y[i_z] = haz_y[i_z] ^ haz_y[16 + i_z];
  }
}

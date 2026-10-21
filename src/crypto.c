/// @file

#include "compat.h"
#include "crypto.h"
#include "blake3.h"
#include "urcrypt.h"

c3_z
mesa_met3_bytes(c3_z len_z, const c3_y* buf_y)
{
  while ( len_z && (0 == buf_y[len_z - 1]) ) {
    len_z--;
  }
  return len_z;
}

/* constant-time comparison
*/
static c3_o
_const_cmp(const c3_y* a_y, const c3_y* b_y, c3_z len_z)
{
  c3_y dif_y = 0;

  for ( c3_z i_z = 0; i_z < len_z; i_z++ ) {
    dif_y |= a_y[i_z] ^ b_y[i_z];
  }
  return __(0 == dif_y);
}

/* blake3
*/
void
mesa_blake3(c3_z len_z, const c3_y* msg_y, c3_z out_z, c3_y* out_y)
{
  blake3_hasher has_u;
  blake3_hasher_init(&has_u);
  blake3_hasher_update(&has_u, msg_y, len_z);
  blake3_hasher_finalize(&has_u, out_y, out_z);
}

void
mesa_blake3_keyed(const c3_y key_y[32],
                  c3_z len_z, const c3_y* msg_y,
                  c3_z out_z, c3_y* out_y)
{
  blake3_hasher has_u;
  blake3_hasher_init_keyed(&has_u, key_y);
  blake3_hasher_update(&has_u, msg_y, len_z);
  blake3_hasher_finalize(&has_u, out_y, out_z);
}

void
mesa_blake3_kdf(const c3_c* ctx_c,
                c3_z len_z, const c3_y* sed_y,
                c3_z out_z, c3_y* out_y)
{
  blake3_hasher has_u;
  blake3_hasher_init_derive_key_raw(&has_u, ctx_c, strlen(ctx_c));
  blake3_hasher_update(&has_u, sed_y, len_z);
  blake3_hasher_finalize(&has_u, out_y, out_z);
}

/* keys
*/
void
mesa_keys_from_seed(mesa_keys* key_u, const c3_y sed_y[64])
{
  memcpy(key_u->sed_y, sed_y, 64);

  //  sgn from (end 8 bod), cry from (cut 8 [1 1] bod)
  //
  urcrypt_ed_luck(sed_y,      key_u->sgn_pub_y, key_u->sgn_sek_y);
  urcrypt_ed_luck(sed_y + 32, key_u->cry_pub_y, key_u->cry_sek_y);
}

c3_o
mesa_keys_from_ring(mesa_keys* key_u, c3_z len_z, const c3_y* rin_y)
{
  c3_y sed_y[64] = {0};

  if ( (0 == len_z) || ('B' != rin_y[0]) || (len_z > 65) ) {
    return c3n;
  }

  memcpy(sed_y, rin_y + 1, len_z - 1);
  mesa_keys_from_seed(key_u, sed_y);
  return c3y;
}

void
mesa_keys_pass(const mesa_keys* key_u, c3_y pas_y[65])
{
  pas_y[0] = 'b';
  memcpy(pas_y + 1,  key_u->sgn_pub_y, 32);
  memcpy(pas_y + 33, key_u->cry_pub_y, 32);
}

c3_o
mesa_pass_parse(c3_z len_z, const c3_y* pas_y,
                c3_y sgn_y[32], c3_y cry_y[32])
{
  c3_y buf_y[65] = {0};

  if ( (0 == len_z) || ('b' != pas_y[0]) || (len_z > 65) ) {
    return c3n;
  }

  memcpy(buf_y, pas_y, len_z);
  memcpy(sgn_y, buf_y + 1,  32);
  memcpy(cry_y, buf_y + 33, 32);
  return c3y;
}

void
mesa_symmetric_key(const c3_y her_cry_y[32],
                   const mesa_keys* our_u,
                   c3_y sym_y[32])
{
  urcrypt_ed_slar(her_cry_y, our_u->cry_sek_y, sym_y);
}

/* ++crypt:crypt
*/
void
mesa_crypt(const c3_y key_y[32],
           c3_z ivl_z, const c3_y* iv_y,
           c3_z len_z, c3_y* msg_y)
{
  c3_y non_y[24];
  c3_y xey_y[32];
  c3_y xon_y[8];

  mesa_blake3_kdf("mesa-crypt-iv", ivl_z, iv_y, 24, non_y);
  urcrypt_chacha_xchacha(8, (c3_y*)key_y, non_y, xey_y, xon_y);
  urcrypt_chacha_crypt(8, xey_y, xon_y, 0, len_z, msg_y);
}

c3_z
mesa_encrypt(const c3_y key_y[32],
             c3_z ivl_z, const c3_y* iv_y,
             c3_z len_z, const c3_y* msg_y,
             c3_y* out_y)
{
  ivl_z = mesa_met3_bytes(ivl_z, iv_y);
  len_z = mesa_met3_bytes(len_z, msg_y);

  memcpy(out_y, msg_y, len_z);
  mesa_crypt(key_y, ivl_z, iv_y, len_z, out_y);
  out_y[len_z] = 0x1;

  return len_z + 1;
}

c3_ds
mesa_decrypt(const c3_y key_y[32],
             c3_z ivl_z, const c3_y* iv_y,
             c3_z len_z, const c3_y* cyf_y,
             c3_y* out_y)
{
  c3_z wid_z;

  ivl_z = mesa_met3_bytes(ivl_z, iv_y);
  len_z = mesa_met3_bytes(len_z, cyf_y);

  if ( (0 == len_z) || (0x1 != cyf_y[len_z - 1]) ) {
    return -1;
  }

  wid_z = len_z - 1;
  memcpy(out_y, cyf_y, wid_z);
  mesa_crypt(key_y, ivl_z, iv_y, wid_z, out_y);

  return (c3_ds)mesa_met3_bytes(wid_z, out_y);
}

/* ++seal-path:crypt / ++open-path:crypt
*/
static void
_aead_keys(const c3_y key_y[32], c3_y enc_y[32], c3_y mac_y[32])
{
  c3_y kez_y[64];

  mesa_blake3_kdf("mesa-aead", 32, key_y, 64, kez_y);
  memcpy(enc_y, kez_y,      32);   //  (end 8 keys)
  memcpy(mac_y, kez_y + 32, 32);   //  (rsh 8 keys)
}

c3_z
mesa_seal_path(const c3_y key_y[32], const c3_c* pat_c, c3_y* out_y)
{
  c3_y enc_y[32], mac_y[32];
  c3_z pat_z = strlen(pat_c);

  _aead_keys(key_y, enc_y, mac_y);

  //  tag
  //
  mesa_blake3_keyed(mac_y, pat_z, (const c3_y*)pat_c, 16, out_y);

  //  cyf
  //
  memcpy(out_y + 16, pat_c, pat_z);
  mesa_crypt(enc_y, 16, out_y, pat_z, out_y + 16);

  out_y[16 + pat_z] = 0x1;
  return 16 + pat_z + 1;
}

c3_ds
mesa_open_path(const c3_y key_y[32],
               c3_z len_z, const c3_y* sel_y,
               c3_c* pat_c, c3_z cap_z)
{
  c3_y  enc_y[32], mac_y[32], tag_y[16], has_y[16];
  c3_z  wid_z, pat_z;

  len_z = mesa_met3_bytes(len_z, sel_y);

  if ( (0 == len_z) || (0x1 != sel_y[len_z - 1]) ) {
    return -1;
  }

  wid_z = len_z - 1;
  if ( wid_z < 16 ) {
    return -1;
  }

  pat_z = wid_z - 16;
  if ( pat_z + 1 > cap_z ) {
    return -1;
  }

  _aead_keys(key_y, enc_y, mac_y);

  memcpy(tag_y, sel_y, 16);
  memcpy(pat_c, sel_y + 16, pat_z);
  mesa_crypt(enc_y, 16, tag_y, pat_z, (c3_y*)pat_c);

  mesa_blake3_keyed(mac_y, pat_z, (const c3_y*)pat_c, 16, has_y);
  if ( c3n == _const_cmp(tag_y, has_y, 16) ) {
    return -1;
  }

  //  (stab q.pat) reads the octs as a cord, dropping trailing NULs
  //
  pat_z = mesa_met3_bytes(pat_z, (c3_y*)pat_c);
  pat_c[pat_z] = 0;
  return (c3_ds)pat_z;
}

/* bindings: (spat path) ++ [32 root]
*/
static c3_y*
_etch_binding(const c3_c* pat_c, const c3_y roo_y[32], c3_z* len_z)
{
  c3_z  pat_z = strlen(pat_c);
  c3_y* bin_y = c3_malloc(pat_z + 32);

  memcpy(bin_y, pat_c, pat_z);
  memcpy(bin_y + pat_z, roo_y, 32);
  *len_z = pat_z + 32;
  return bin_y;
}

void
mesa_sign_binding(const mesa_keys* key_u,
                  const c3_c* pat_c,
                  const c3_y roo_y[32],
                  c3_y sig_y[64])
{
  c3_z  len_z;
  c3_y* bin_y = _etch_binding(pat_c, roo_y, &len_z);

  urcrypt_ed_sign_raw(bin_y, len_z, key_u->sgn_pub_y, key_u->sgn_sek_y, sig_y);
  c3_free(bin_y);
}

c3_o
mesa_verify_binding(const c3_y pub_y[32],
                    const c3_y sig_y[64],
                    const c3_c* pat_c,
                    const c3_y roo_y[32])
{
  c3_z  len_z;
  c3_y* bin_y = _etch_binding(pat_c, roo_y, &len_z);
  c3_o  ver_o = __(urcrypt_ed_veri(bin_y, len_z, pub_y, sig_y));

  c3_free(bin_y);
  return ver_o;
}

void
mesa_mac_binding(const c3_y key_y[32],
                 const c3_c* pat_c,
                 const c3_y roo_y[32],
                 c3_y mac_y[16])
{
  c3_z  len_z;
  c3_y* bin_y = _etch_binding(pat_c, roo_y, &len_z);

  mesa_blake3_keyed(key_y, len_z, bin_y, 16, mac_y);
  c3_free(bin_y);
}

c3_o
mesa_verify_mac(const c3_y key_y[32],
                const c3_y mac_y[16],
                const c3_c* pat_c,
                const c3_y roo_y[32])
{
  c3_y has_y[16];

  mesa_mac_binding(key_y, pat_c, roo_y, has_y);
  return _const_cmp(mac_y, has_y, 16);
}

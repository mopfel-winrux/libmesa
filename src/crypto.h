/// @file
///
/// Mesa cryptography, mirroring ++crypt and friends in
/// urbit/pkg/arvo/sys/vane/ames.hoon and ++ed / ++blake3 / ++chacha in
/// zuse.hoon.
///
/// Byte conventions follow Hoon octs: a byte buffer is the little-endian
/// encoding of an atom, with an explicit length. Where Hoon measures an
/// atom with (met 3 ...) instead of carrying a length, the caller must
/// pass the trimmed length (see mesa_met3_bytes()).

#ifndef MESA_CRYPTO_H
#define MESA_CRYPTO_H

#include "c3/c3.h"

/* mesa_keys: a suite %b networking keypair (+keypairs:ames).
**
**   sek_y fields are ed25519 expanded secret keys (+luck:ed), 64 bytes.
*/
typedef struct _mesa_keys {
  c3_y sed_y[64];     //  ring body: sgn seed (0..32) ++ cry seed (32..64)
  c3_y sgn_pub_y[32];
  c3_y sgn_sek_y[64];
  c3_y cry_pub_y[32];
  c3_y cry_sek_y[64];
} mesa_keys;

/* mesa_met3_bytes(): (met 3 a) for a little-endian byte buffer
*/
c3_z
mesa_met3_bytes(c3_z len_z, const c3_y* buf_y);

/* blake3 (+blake3:blake:crypto)
*/
void
mesa_blake3(c3_z len_z, const c3_y* msg_y, c3_z out_z, c3_y* out_y);

void
mesa_blake3_keyed(const c3_y key_y[32],
                  c3_z len_z, const c3_y* msg_y,
                  c3_z out_z, c3_y* out_y);

void
mesa_blake3_kdf(const c3_c* ctx_c,
                c3_z len_z, const c3_y* sed_y,
                c3_z out_z, c3_y* out_y);

/* mesa_keys_from_ring(): +nol:nu:cric for a suite %b ring ('B' ++ 64 bytes)
**
**   [rin_y] is the ring atom's 65 little-endian bytes; c3n if not suite %b.
*/
c3_o
mesa_keys_from_ring(mesa_keys* key_u, c3_z len_z, const c3_y* rin_y);

/* mesa_keys_from_seed(): suite %b keys from a 64-byte ring body
*/
void
mesa_keys_from_seed(mesa_keys* key_u, const c3_y sed_y[64]);

/* mesa_keys_pass(): +pub:ex:cric, 'b' ++ sgn ++ cry (65 bytes)
*/
void
mesa_keys_pass(const mesa_keys* key_u, c3_y pas_y[65]);

/* mesa_pass_parse(): split a suite %b pass into its public keys; c3n if
**                    not suite %b.
*/
c3_o
mesa_pass_parse(c3_z len_z, const c3_y* pas_y,
                c3_y sgn_y[32], c3_y cry_y[32]);

/* mesa_symmetric_key(): +derive-symmetric-key, (slar:ed cry.pub cry.sek)
*/
void
mesa_symmetric_key(const c3_y her_cry_y[32],
                   const mesa_keys* our_u,
                   c3_y sym_y[32]);

/* mesa_crypt(): ++crypt:crypt, XChaCha8 keyed by [key], nonce derived
**               from [iv]; encrypts or decrypts [msg_y] in place.
*/
void
mesa_crypt(const c3_y key_y[32],
           c3_z ivl_z, const c3_y* iv_y,
           c3_z len_z, c3_y* msg_y);

/* mesa_encrypt(): ++encrypt:crypt
**
**   [iv] and [msg] are atoms (lengths are trimmed with met 3). Writes
**   (met 3 msg) + 1 bytes to [out_y] and returns that length.
*/
c3_z
mesa_encrypt(const c3_y key_y[32],
             c3_z ivl_z, const c3_y* iv_y,
             c3_z len_z, const c3_y* msg_y,
             c3_y* out_y);

/* mesa_decrypt(): ++decrypt:crypt
**
**   [cyf] is an atom; writes up to (met 3 cyf) - 1 bytes to [out_y] and
**   returns the plaintext's (met 3) length, or -1 if malformed.
*/
c3_ds
mesa_decrypt(const c3_y key_y[32],
             c3_z ivl_z, const c3_y* iv_y,
             c3_z len_z, const c3_y* cyf_y,
             c3_y* out_y);

/* mesa_seal_path(): ++seal-path:crypt
**
**   [pat_c] is the rendered path, e.g. "/a/x/1//flow/0/poke/for/~zod/1".
**   Writes 16 + strlen(pat_c) + 1 bytes to [out_y]; returns that length.
*/
c3_z
mesa_seal_path(const c3_y key_y[32], const c3_c* pat_c, c3_y* out_y);

/* mesa_open_path(): ++open-path:crypt
**
**   [sel] is the sealed atom. Writes the NUL-terminated rendered path to
**   [pat_c] (capacity [cap_z]); returns its length or -1 if forged or
**   malformed.
*/
c3_ds
mesa_open_path(const c3_y key_y[32],
               c3_z len_z, const c3_y* sel_y,
               c3_c* pat_c, c3_z cap_z);

/* mesa_sign_binding(): ++sign:crypt, ed25519 over (path text ++ root)
*/
void
mesa_sign_binding(const mesa_keys* key_u,
                  const c3_c* pat_c,
                  const c3_y roo_y[32],
                  c3_y sig_y[64]);

/* mesa_verify_binding(): ++verify-sig:crypt
*/
c3_o
mesa_verify_binding(const c3_y pub_y[32],
                    const c3_y sig_y[64],
                    const c3_c* pat_c,
                    const c3_y roo_y[32]);

/* mesa_mac_binding(): ++mac:crypt, 16-byte keyed blake3 over the binding
*/
void
mesa_mac_binding(const c3_y key_y[32],
                 const c3_c* pat_c,
                 const c3_y roo_y[32],
                 c3_y mac_y[16]);

/* mesa_verify_mac(): ++verify-mac:crypt (constant time)
*/
c3_o
mesa_verify_mac(const c3_y key_y[32],
                const c3_y mac_y[16],
                const c3_c* pat_c,
                const c3_y roo_y[32]);

#endif /* ifndef MESA_CRYPTO_H */

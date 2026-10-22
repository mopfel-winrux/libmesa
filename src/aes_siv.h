/// @file
///
/// AES-256-SIV, as Hoon's ++sivc:aes:crypto (classic ames' cipher).
///
/// All byte arrays are little-endian atom bytes, as in vere: the 64-byte
/// key (zero-padded), each associated datum with its length (+met 3), the
/// message, the 16-byte synthetic IV and the ciphertext (as long as the
/// message).

#ifndef MESA_AES_SIV_H
#define MESA_AES_SIV_H

#include "c3/c3.h"

void
mesa_sivc_en(const c3_y key_y[64], c3_w ads_w, const c3_y** ads_y,
             const c3_z* adz_z, const c3_y* msg_y, c3_z msg_z,
             c3_y iv_y[16], c3_y* cyf_y);

/* mesa_sivc_de(): c3n if [iv_y] doesn't authenticate
*/
c3_o
mesa_sivc_de(const c3_y key_y[64], c3_w ads_w, const c3_y** ads_y,
             const c3_z* adz_z, const c3_y iv_y[16], const c3_y* cyf_y,
             c3_z cyf_z, c3_y* msg_y);

#endif /* ifndef MESA_AES_SIV_H */

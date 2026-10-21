/// @file
///
/// The +sha family from hoon.hoon. Byte conventions follow the jets: an
/// atom's little-endian bytes are hashed, and a digest's bytes are the
/// little-endian bytes of the resulting atom.

#ifndef MESA_SHA_H
#define MESA_SHA_H

#include "c3/c3.h"

/* mesa_shay(): +shay, sha-256 of [len_z] bytes
*/
void
mesa_shay(c3_z len_z, const c3_y* msg_y, c3_y out_y[32]);

/* mesa_shax(): +shax, sha-256 of an atom (trailing zeros trimmed)
*/
void
mesa_shax(c3_z len_z, const c3_y* msg_y, c3_y out_y[32]);

/* mesa_shal(): +shal, sha-512 of [len_z] bytes
*/
void
mesa_shal(c3_z len_z, const c3_y* msg_y, c3_y out_y[64]);

/* mesa_shas(): +shas, salted sha-256; salt and message are atoms
*/
void
mesa_shas(c3_z sal_z, const c3_y* sal_y,
          c3_z len_z, const c3_y* msg_y,
          c3_y out_y[32]);

/* mesa_shaf(): +shaf, half sha-256 (shas folded to 128 bits)
*/
void
mesa_shaf(c3_z sal_z, const c3_y* sal_y,
          c3_z len_z, const c3_y* msg_y,
          c3_y out_y[16]);

#endif /* ifndef MESA_SHA_H */

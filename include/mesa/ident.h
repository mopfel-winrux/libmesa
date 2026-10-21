/// @file
///
/// Ship identities: who we are on the network and the keys that prove it.
///
/// Every identity is a ship, its rift (continuity number), its life (key
/// revision) and a suite %b ring ('B' ++ 64-byte seed). Planets, stars
/// and galaxies load theirs from a keyfile; moons from the keyfile their
/// parent's |moon generates; comets are mined; fake ships derive theirs
/// from the ship name (only fake ships will talk to them).

#ifndef MESA_IDENT_H
#define MESA_IDENT_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* mesa_ship: a ship, as a 128-bit little-endian integer
*/
typedef struct mesa_ship {
  uint64_t lo;
  uint64_t hi;
} mesa_ship;

typedef enum mesa_rank {
  MESA_CZAR = 0,      //  galaxy,  8 bits
  MESA_KING = 1,      //  star,   16 bits
  MESA_DUKE = 2,      //  planet, 32 bits
  MESA_EARL = 3,      //  moon,   64 bits
  MESA_PAWN = 4,      //  comet, 128 bits
} mesa_rank;

/* MESA_RIFT_UNKNOWN: the keyfile didn't say; resolve it from azimuth
*/
#define MESA_RIFT_UNKNOWN UINT64_MAX

typedef struct mesa_ident {
  mesa_ship who;
  uint64_t  rift;
  uint64_t  life;
  uint8_t   ring[65];   //  'B' ++ sgn seed (32) ++ cry seed (32)
} mesa_ident;

/* ships
*/
bool      mesa_ship_parse(const char* str, mesa_ship* who);   //  "~zod"
char*     mesa_ship_render(mesa_ship who);                     //  malloc'd
mesa_rank mesa_ship_rank(mesa_ship who);
mesa_ship mesa_ship_sein(mesa_ship who);    //  default sponsor
bool      mesa_ship_eq(mesa_ship a, mesa_ship b);

/* mesa_ident_pass(): our public key, 'b' ++ sgn ++ cry (65 bytes)
*/
void
mesa_ident_pass(const mesa_ident* ide, uint8_t pass[65]);

/* mesa_keyfile_parse(): parse a keyfile (@uw text of a jammed
**                       +feed:jael or +seed:jael).
**
**   Writes up to [cap] identities to [out] (a feed may carry several
**   lives) and returns how many it found, or -1 if the text isn't a
**   keyfile. [rift] is MESA_RIFT_UNKNOWN unless the keyfile says.
*/
int
mesa_keyfile_parse(const char* text, mesa_ident* out, size_t cap);

/* mesa_keyfile_render(): render [ide] as a [%2 ~] feed keyfile (malloc'd).
**
**   Use it to save a mined comet; an unknown rift is written as 0.
*/
char*
mesa_keyfile_render(const mesa_ident* ide);

/* mesa_ident_fake(): fake-ship keys, (pit:nu:cric 512 who %b ~).
**
**   Rift 0, life 1; matches `urbit -F`.
*/
void
mesa_ident_fake(mesa_ship who, mesa_ident* out);

/* mesa_comet_mine(): +come:dawn, mine a comet under one of [stars].
**
**   Tries seeds eny, eny+1, ... up to [max_tries] times. [eny] must be 64
**   bytes of fresh randomness. Returns false if [stars] contains a
**   non-star or nothing was found.
*/
bool
mesa_comet_mine(size_t len,
                const mesa_ship* stars,
                const uint8_t eny[64],
                uint64_t max_tries,
                mesa_ident* out);

/* mesa_comet_check(): is [pass] a valid key for the comet [who]?
**
**   A comet's address is the fingerprint of its key, and its default
**   sponsor must be a star.
*/
bool
mesa_comet_check(mesa_ship who, const uint8_t pass[65]);

#endif /* ifndef MESA_IDENT_H */

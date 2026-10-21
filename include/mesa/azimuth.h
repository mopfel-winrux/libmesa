/// @file
///
/// Peer keys from azimuth, via the azimuth-rpc JSON-RPC API that the
/// Urbit roller serves (default https://roller.urbit.org/v1/azimuth).
///
/// These functions only build requests and parse responses; the host
/// does the HTTP POST (Content-Type: application/json) with whatever
/// client it likes. This mirrors +point:give:dawn and +point:take:dawn
/// in urbit/pkg/arvo/lib/vere.hoon.

#ifndef MESA_AZIMUTH_H
#define MESA_AZIMUTH_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "mesa/ident.h"

#define MESA_AZIMUTH_URL "https://roller.urbit.org/v1/azimuth"

/* mesa_point: a ship's networking state (+point:jael, minus history)
**
**   [life] 0 means the ship has no networking keys yet; [pass] is then
**   all zeros after the 'b'.
*/
typedef struct mesa_point {
  mesa_ship who;
  uint64_t  rift;
  uint64_t  life;
  uint8_t   pass[65];     //  'b' ++ auth ++ crypt
  bool      has_sponsor;
  mesa_ship sponsor;
} mesa_point;

typedef enum mesa_azimuth_err {
  MESA_AZIMUTH_OK        =  0,
  MESA_AZIMUTH_NOT_FOUND =  1,  //  not spawned (RPC error -32000)
  MESA_AZIMUTH_BAD       = -1,  //  malformed or unexpected response
} mesa_azimuth_err;

/* mesa_azimuth_point_request(): JSON body for getPoint (malloc'd)
*/
char*
mesa_azimuth_point_request(mesa_ship who);

/* mesa_azimuth_point_parse(): parse a getPoint response for [who]
*/
mesa_azimuth_err
mesa_azimuth_point_parse(mesa_ship who,
                         size_t len,
                         const char* json,
                         mesa_point* out);

/* mesa_azimuth_dns_request(): JSON body for getDns (malloc'd)
*/
char*
mesa_azimuth_dns_request(void);

/* mesa_azimuth_dns_parse(): parse a getDns response.
**
**   Returns the number of domains (e.g. "urbit.org"; galaxy ~zod is then
**   reached at zod.urbit.org) and writes up to [cap] malloc'd strings to
**   [out]; -1 if malformed.
*/
int
mesa_azimuth_dns_parse(size_t len, const char* json, char** out, size_t cap);

#endif /* ifndef MESA_AZIMUTH_H */

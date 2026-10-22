/// @file
///
/// A ready-made host for the engine: a libuv event loop with a UDP
/// socket, timers, galaxy DNS and the azimuth key resolver (HTTPS via
/// libcurl). Link libmesa-host (and its libuv, curl and OpenSSL) to use
/// it; the core library has no I/O of its own.
///
/// Typical use:
///
///   mesa_host_config cfg = { .engine = { .ident = ide, .store = sto,
///                                        .hooks = my_hooks } };
///   mesa_host* hos;
///   if ( MESA_OK == mesa_host_open(&cfg, &hos) ) {
///     mesa_engine* eng = mesa_host_engine(hos);
///     ...start flows...
///     mesa_host_run(hos);
///   }
///
/// The host fills in hooks.send and hooks.need_point; the application's
/// other hooks are passed through. Everything runs on the loop's thread.

#ifndef MESA_HOST_H
#define MESA_HOST_H

#include "mesa/engine.h"

typedef struct mesa_host mesa_host;

typedef struct mesa_host_config {
  mesa_config engine;

  /* port: UDP port to bind. 0: galaxies use their well-known port,
  **       everyone else an ephemeral one.
  */
  uint16_t    port;

  /* fake: talk to fake ships (`urbit -F`) on localhost: galaxy ~g is at
  **       127.0.0.1:31337+g and every ship has fake keys.
  */
  bool        fake;

  const char* azimuth_url;   //  default MESA_AZIMUTH_URL
  const char* dns_domain;    //  default "urbit.org"

  /* loop: run on this uv_loop_t* instead of the default loop
  */
  void*       loop;
} mesa_host_config;

typedef enum mesa_host_err {
  MESA_HOST_E_SOCKET   = -100,  //  can't bind the UDP port
  MESA_HOST_E_AZIMUTH  = -101,  //  can't resolve our own point
  MESA_HOST_E_KEYS     = -102,  //  our keys don't match azimuth
} mesa_host_err;

/* mesa_host_open(): resolve our own point (unless fake, a comet or a
**                   moon), check our keys against it, open the engine
**                   and bind the socket. Returns a mesa_err or
**                   mesa_host_err.
*/
int          mesa_host_open(const mesa_host_config* cfg, mesa_host** out);
mesa_engine* mesa_host_engine(mesa_host* hos);
void*        mesa_host_loop(mesa_host* hos);

/* mesa_host_kick(): call after using the engine API from outside a
**                   hook, so the host re-arms its timer
*/
void         mesa_host_kick(mesa_host* hos);

int          mesa_host_run(mesa_host* hos);    //  uv_run(UV_RUN_DEFAULT)
void         mesa_host_stop(mesa_host* hos);
void         mesa_host_close(mesa_host* hos);

/* mesa_azimuth_fetch(): blocking getPoint over HTTPS (for tools)
*/
mesa_azimuth_err
mesa_azimuth_fetch(const char* url, mesa_ship who, mesa_point* out);

/* mesa_host_get(): blocking HTTPS GET (for tools); malloc'd body or 0
** mesa_host_post(): blocking POST; true on a 2xx answer. Thread-safe;
**                   run it off the loop (uv_queue_work).
*/
uint8_t*
mesa_host_get(const char* url, size_t* len);
bool
mesa_host_post(const char* url, const char* type, const void* body, size_t len);

#endif /* ifndef MESA_HOST_H */

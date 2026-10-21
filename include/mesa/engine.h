/// @file
///
/// The mesa protocol engine.
///
/// The engine is sans-IO: the host feeds it received datagrams, timer
/// expirations and peer key information, and the engine calls back to
/// send datagrams and to deliver messages. It is single-threaded; call
/// it from one thread only.
///
/// Every input runs as one transaction against the store (see
/// mesa/store.h). Message handlers (hooks.plea, hooks.boon, hooks.done)
/// run inside that transaction, and datagrams are only handed to
/// hooks.send after it commits. An application that keeps its state in
/// the same store therefore processes each message exactly once: a crash
/// before commit loses both the application's change and the ack, and
/// the peer resends.
///
/// Flows are Arvo's: a forward flow carries our %pleas (requests) to a
/// peer and their %boons (responses, e.g. subscription updates) back; a
/// backward flow carries their %pleas to us and our %boons to them. Each
/// flow is a bone; messages on a flow are delivered in order, one at a
/// time.
///
/// Nouns passed to the engine are TRANSFERRED; nouns passed to hooks are
/// BORROWED for the duration of the call.

#ifndef MESA_ENGINE_H
#define MESA_ENGINE_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "mesa/azimuth.h"
#include "mesa/ident.h"
#include "mesa/noun.h"
#include "mesa/store.h"

typedef struct mesa_engine mesa_engine;

/* mesa_lane: where to send a datagram
**
**   MESA_LANE_GALAXY means "galaxy [galaxy]'s address"; the host resolves
**   it (e.g. zod.urbit.org:13337, or 127.0.0.1:31337 for fake ships).
*/
typedef enum mesa_lane_kind {
  MESA_LANE_NONE   = 0,
  MESA_LANE_GALAXY = 1,
  MESA_LANE_IP     = 2,
} mesa_lane_kind;

typedef struct mesa_lane {
  mesa_lane_kind kind;
  uint8_t        galaxy;
  uint32_t       ip;      //  IPv4, host byte order
  uint16_t       port;
} mesa_lane;

/* mesa_plea_result: what a plea handler did with a message
*/
typedef enum mesa_plea_result {
  MESA_ACK   = 0,   //  handled; ack it
  MESA_NACK  = 1,   //  refused; its writes are rolled back, *error is sent
  MESA_LATER = 2,   //  will call mesa_flow_done() later
} mesa_plea_result;

typedef struct mesa_hooks {
  void* ctx;

  /* send(): transmit one datagram (after the event commits)
  */
  void (*send)(void* ctx, mesa_lane lane, const uint8_t* buf, size_t len);

  /* need_point(): please resolve [who]'s keys and sponsor, then call
  **               mesa_engine_point() or mesa_engine_point_missing().
  */
  void (*need_point)(void* ctx, mesa_ship who);

  /* plea(): a request from [her] on our backward flow [bone]
  **
  **   [vane] is a @tas (e.g. %g), [path] a path (e.g. /ge/my-agent),
  **   [payload] the request. On MESA_NACK, set *error to an error noun
  **   ([tag=@tas tang]) or leave it MESA_NONE for a default.
  */
  mesa_plea_result (*plea)(void* ctx, mesa_ship her, uint64_t bone,
                           mesa_noun vane, mesa_noun path, mesa_noun payload,
                           mesa_noun* error);

  /* boon(): a response from [her] on our forward flow [bone]
  */
  void (*boon)(void* ctx, mesa_ship her, uint64_t bone, mesa_noun payload);

  /* done(): our plea on [bone] was acked ([error] is MESA_NONE) or
  **         nacked ([error] is their naxplanation).
  */
  void (*done)(void* ctx, mesa_ship her, uint64_t bone, mesa_noun error);

  /* corked(): our forward flow [bone] is closed for good.
  */
  void (*corked)(void* ctx, mesa_ship her, uint64_t bone);

  /* sage(): result of mesa_keen(); [gage] is [mark noun], or MESA_NONE
  **         if the request was cancelled.
  */
  void (*sage)(void* ctx, mesa_ship her, const char* path, mesa_noun gage);

  /* peek(): serve our namespace. [path] is the inner path, e.g.
  **         "/g/x/0/my-agent//some/path". [her] is the reader for
  **         encrypted (two-party) reads, NULL for public ones. Return a
  **         [mark noun] cell, or MESA_NONE if there's nothing there.
  **         Bindings must be immutable: once a path has a value, it must
  **         always have the same value.
  */
  mesa_noun (*peek)(void* ctx, const mesa_ship* her, const char* path);

  /* breach(): [her] breached (rift changed); all flows with them are
  **           gone, and so is any unacked message.
  */
  void (*breach)(void* ctx, mesa_ship her);
} mesa_hooks;

typedef struct mesa_config {
  mesa_ident  ident;
  mesa_store* store;
  mesa_hooks  hooks;

  /* our_sponsor: our sponsor (from azimuth for stars and planets, our
  **              parent for moons); ignored for comets and galaxies,
  **              whose sponsor is fixed.
  */
  mesa_ship   our_sponsor;

  /* breached: allow starting this identity with an empty store.
  **
  **   Peers keep per-ship state (sequence numbers, acks); talking to them
  **   from fresh state looks like replaying old messages. An identity may
  **   start fresh only if it never ran before, or has breached (its rift
  **   went up) since it last ran. Comets and fake ships never need this.
  */
  bool        breached;

  /* ping: keep our galaxy's route to us fresh (poke its %ping agent
  **       every 25s, as /app/ping does). On by default for non-galaxies.
  */
  bool        no_ping;

  uint64_t    now;          //  milliseconds since the epoch
} mesa_config;

typedef enum mesa_err {
  MESA_OK          =  0,
  MESA_E_STORE     = -1,  //  store failed
  MESA_E_FRESH     = -2,  //  empty store, and not told we breached
  MESA_E_IDENTITY  = -3,  //  store belongs to another ship/life/rift
  MESA_E_ARG       = -4,  //  bad argument
  MESA_E_STATE     = -5,  //  flow can't take this now (closing, corked)
  MESA_E_FAILED    = -6,  //  engine stopped after a store failure
} mesa_err;

/* lifecycle
*/
mesa_err mesa_engine_open(const mesa_config* cfg, mesa_engine** out);
void     mesa_engine_close(mesa_engine* eng);

/* inputs
*/
void     mesa_engine_hear(mesa_engine* eng, mesa_lane from,
                          const uint8_t* buf, size_t len, uint64_t now);
void     mesa_engine_wake(mesa_engine* eng, uint64_t now);
uint64_t mesa_engine_deadline(mesa_engine* eng);  //  ms; UINT64_MAX: none

void     mesa_engine_point(mesa_engine* eng, const mesa_point* pot);
void     mesa_engine_point_missing(mesa_engine* eng, mesa_ship who);

/* galaxy routing: galaxies are always reachable at their own lane.
*/
mesa_ship mesa_engine_ship(mesa_engine* eng);

/* application transactions
**
**   Group application writes (in the same store) with engine calls so
**   they commit together. Engine calls made outside a transaction run in
**   their own. Hooks already run inside one; don't begin another there.
*/
mesa_err mesa_txn_begin(mesa_engine* eng);
mesa_err mesa_txn_commit(mesa_engine* eng);
void     mesa_txn_abort(mesa_engine* eng);

/* flows
*/
mesa_err mesa_flow_open(mesa_engine* eng, mesa_ship her, uint64_t* bone);
mesa_err mesa_flow_plea(mesa_engine* eng, mesa_ship her, uint64_t bone,
                        mesa_noun vane, mesa_noun path, mesa_noun payload);
mesa_err mesa_flow_cork(mesa_engine* eng, mesa_ship her, uint64_t bone);
mesa_err mesa_flow_boon(mesa_engine* eng, mesa_ship her, uint64_t bone,
                        mesa_noun payload);
mesa_err mesa_flow_done(mesa_engine* eng, mesa_ship her, uint64_t bone,
                        mesa_noun error);

/* remote scry: read [path] (e.g. "/g/x/0/agent//foo") from [her]'s
**              public namespace; the answer arrives via hooks.sage.
*/
mesa_err mesa_keen(mesa_engine* eng, mesa_ship her, const char* path);
mesa_err mesa_yawn(mesa_engine* eng, mesa_ship her, const char* path);

#endif /* ifndef MESA_ENGINE_H */

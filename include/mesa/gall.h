/// @file
///
/// Gall's protocol over mesa flows: what a ship's gall sends and expects
/// (+mo-send-foreign-request and $ames-response in gall.hoon).
///
/// Talking to an agent: open a flow per subscription (or one for pokes),
/// then send requests on it. Acks arrive via hooks.done: for a poke,
/// the poke-ack; for a watch, the watch-ack. Facts and kicks arrive via
/// hooks.boon; decode them with mesa_gall_response(). Gall corks a
/// subscription's flow after leaving it; do the same with
/// mesa_flow_cork() once the leave is acked.
///
/// Serving: a ship's gall pokes or watches "our agents" with the same
/// pleas; decode them in hooks.plea with mesa_gall_request_parse(), and send
/// facts and kicks on that bone with mesa_gall_fact()/mesa_gall_kick().

#ifndef MESA_GALL_H
#define MESA_GALL_H

#include "mesa/engine.h"
#include "mesa/noun.h"

/* requests to a remote agent (nouns transferred)
*/
mesa_err mesa_gall_poke(mesa_engine* eng, mesa_ship her, uint64_t bone,
                        const char* agent, const char* mark, mesa_noun noun);
mesa_err mesa_gall_watch(mesa_engine* eng, mesa_ship her, uint64_t bone,
                         const char* agent, mesa_noun path);
mesa_err mesa_gall_leave(mesa_engine* eng, mesa_ship her, uint64_t bone,
                         const char* agent);

/* responses on a forward flow (borrowed; outputs borrowed from it)
*/
typedef enum mesa_gall_response_kind {
  MESA_GALL_BAD  = 0,
  MESA_GALL_FACT = 1,     //  [%d mark noun]
  MESA_GALL_KICK = 2,     //  [%x ~]
} mesa_gall_response_kind;

mesa_gall_response_kind
mesa_gall_response(mesa_noun boon, mesa_noun* mark, mesa_noun* noun);

/* requests to us, from a plea (borrowed; outputs borrowed from it)
*/
typedef enum mesa_gall_request_kind {
  MESA_GALL_NONE     = 0,  //  not a gall request we understand
  MESA_GALL_POKE     = 1,  //  [%m mark noun]
  MESA_GALL_WATCH    = 2,  //  [%s path]
  MESA_GALL_WATCH_AS = 3,  //  [%l mark path]
  MESA_GALL_LEAVE    = 4,  //  [%u ~]
  MESA_GALL_SYSTEM   = 5,  //  gall's own flows (/gf flubs, /gk keys); the
                           //  engine answers these itself, so hooks.plea
                           //  never sees them
} mesa_gall_request_kind;

typedef struct mesa_gall_request {
  mesa_gall_request_kind kind;
  char                   agent[64];
  mesa_noun              mark;    //  POKE, WATCH_AS
  mesa_noun              noun;    //  POKE
  mesa_noun              path;    //  WATCH, WATCH_AS
} mesa_gall_request;

mesa_gall_request_kind
mesa_gall_request_parse(mesa_noun vane, mesa_noun path, mesa_noun payload,
                        mesa_gall_request* req);

/* responses from us on a backward flow (nouns transferred)
*/
mesa_err mesa_gall_fact(mesa_engine* eng, mesa_ship her, uint64_t bone,
                        const char* mark, mesa_noun noun);
mesa_err mesa_gall_kick(mesa_engine* eng, mesa_ship her, uint64_t bone);

#endif /* ifndef MESA_GALL_H */

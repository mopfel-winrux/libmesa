/// @file
///
/// Engine internals. The structure mirrors the |mesa core of
/// urbit/pkg/arvo/sys/vane/ames.hoon (fren-state, flow-state, the .pit)
/// merged with the packet layer of vere/pkg/vere/io/mesa.c (fragment
/// fetching, congestion control, the responder-side pending-interest
/// table), since here both live in one process.

#ifndef MESA_ENG_H
#define MESA_ENG_H

#include "c3/c3.h"
#include "compat.h"
#include "crypto.h"
#include "knot.h"
#include "ship.h"
#include "mesa.h"
#include "lss.h"
#include "mesa/engine.h"
#include "mesa/noun.h"

#define ENG_MTU          1472    //  max-mtu
#define ENG_FRAG         1024    //  bytes per fragment (bloq 13)
#define ENG_BOQ          13
#define ENG_MAX_MSG      (64u << 20)  //  largest message we'll fetch

#define ENG_RTO_MIN      1000        //  ms
#define ENG_RTO_MAX      120000      //  ~m2, ames.hoon's retry-timer
#define ENG_DEAD_MS      30000       //  ~s30, +is-peer-dead
#define ENG_PIT_MS       20000       //  responder PIT expiry (vere)
#define ENG_PING_MS      25000       //  /app/ping nat-timeout
#define ENG_POINT_MS     3600000     //  re-resolve active peers' keys
#define ENG_CACHE_MAX    (64u << 20) //  served-message cache, bytes
#define ENG_ALIEN_MAX    4096        //  peers without keys, at once
#define ENG_STASH_MAX    8           //  their pokes we keep, each
#define ENG_IDLE_MS      600000      //  unload a quiet peer from memory
#define ENG_GC_MS        60000       //  how often we look for them

/* dire: %for (we send pleas) or %bak (we send boons)
*/
typedef enum { DIRE_FOR = 0, DIRE_BAK = 1 } eng_dire;

/* message: a queued outbound message (+mesa-message), with its
**          serialized page cached once built
*/
typedef struct _eng_msg {
  c3_d             seq_d;
  mesa_noun        msg;      //  [%plea vane path payload] or [%boon payload]
  struct _eng_msg* nex_u;
} eng_msg;

typedef struct _eng_nax {
  c3_d      seq_d;
  mesa_noun err;
} eng_nax;

/* flow: +flow-state
*/
typedef struct _eng_flow {
  c3_d      bon_d;
  eng_dire  dir_e;
  c3_o      clo_o;           //  closing
  c3_o      hat_o;           //  halt
  mesa_noun hag;             //  backward: the agent we flubbed for, or NONE
  //  snd
  c3_d      nex_d;           //  next seq to assign (starts at 1)
  c3_w      win_w;           //  send-window (max 1)
  eng_msg*  lod_u;           //  unacked messages, ascending seq
  eng_msg*  lot_u;           //    the last
  //  rcv
  c3_d      las_d;           //  last-acked
  c3_o      pen_o;           //  pending-ack
  eng_nax   nax_u[10];       //  last ten nacks
  c3_w      nan_w;
} eng_flow;

/* request kinds: what a .pit entry is for
*/
typedef enum {
  REQ_ACK   = 1,   //  ack for our poke on a flow (payload = our message)
  REQ_NAXP  = 2,   //  naxplanation for a nacked plea
  REQ_CORK  = 3,   //  their %cork on a closing backward flow
  REQ_POKE  = 4,   //  payload of a multi-fragment poke to us
  REQ_KEEN  = 5,   //  application remote scry
  REQ_PROOF = 6,   //  comet attestation
} eng_req_e;

/* fetch: in-progress multi-fragment download (vere's u3_pend_req)
*/
typedef struct _eng_fetch {
  c3_d          tob_d;       //  total bytes
  c3_d          tof_d;       //  total fragments
  c3_d          nex_d;       //  next fragment to request
  c3_d          hav_d;       //  fragments verified (in order)
  c3_d          out_d;       //  in flight
  c3_d          fas_d;       //  fast-loss cursor: next to check
  c3_d*         rsn_d;       //  fragments re-requested, not yet back
  c3_w          rsn_w, rsc_w;
  c3_y*         dat_y;       //  tob_d bytes
  c3_y*         got_y;       //  per-fragment: received
  c3_d*         sen_d;       //  per-fragment: last sent (ms)
  c3_d*         sno_d;       //  per-fragment: last sent (peer send number)
  lss_pair*     par_u;       //  per-fragment pairs (for misordered)
  c3_y*         pap_y;       //  per-fragment: has pair
  lss_verifier  los_u;
  arena         are_u;
} eng_fetch;

typedef struct _eng_req {
  c3_c*             pat_c;   //  full namespace path, no leading '/'
  eng_req_e         typ_e;
  c3_d              bon_d;   //  flow (REQ_ACK/NAXP/CORK/POKE)
  eng_dire          dir_e;
  c3_d              seq_d;
  c3_c*             pay_c;   //  REQ_ACK: our payload path (poke packet)
  c3_c*             key_c;   //  REQ_KEEN: the path the app asked for
  c3_o              shu_o;   //  REQ_KEEN: a %shut read, with this key
  c3_d              kid_d;
  c3_y              shk_y[32];
  //  retry
  c3_d              nex_d;   //  next (re)send, ms
  c3_d              rto_d;
  c3_w              try_w;   //  sends so far
  //  REQ_KEEN: the first request takes a slot in the peer's window
  c3_o              wai_o;   //  waiting for a slot
  c3_o              fly_o;   //  has one
  c3_d              sen_d;   //  last sent (ms)
  c3_d              sno_d;   //    (peer send number)
  //  fetch state, once we've heard the first page
  eng_fetch*        fet_u;
  c3_o              aut_o;   //  first page authenticated
  struct _eng_req*  nex_u;
  struct _eng_req*  pre_u;
} eng_req;

#define NAME     eng_req_map
#define KEY_TY   c3_c*         //  the request's pat_c
#define VAL_TY   eng_req*
#include "verstable.h"

typedef struct _eng_peer eng_peer;

#define NAME     eng_flow_map
#define KEY_TY   c3_d          //  (bone << 1) | dire
#define VAL_TY   eng_flow*
#include "verstable.h"

/* corked backward flows: ranges of bones, one list per bone mod 4
**
**   [lo hi] holds lo, lo+4, ... hi. Our own corked forward flows aren't
**   kept: they're our bones below next-bone with no flow.
*/
typedef struct { c3_d lo_d, hi_d; } eng_ran;
typedef struct {
  eng_ran* ran_u;
  c3_w     len_w, cap_w;
} eng_crk;

/* datagrams: outbound, released after commit; or kept to hear later
*/
typedef struct _eng_out {
  mesa_lane        lan_u;
  c3_z             len_z;
  c3_y*            buf_y;
  struct _eng_out* nex_u;
} eng_out;

struct _eng_peer {
  u3_ship       who_u;
  c3_o          kno_o;       //  have keys (%known vs %alien)
  c3_o          ask_o;       //  point request outstanding
  c3_d          ask_d;       //  when we last asked (ms)
  //  azimuth-state
  c3_d          rif_d;
  c3_d          lyf_d;
  c3_y          sgn_y[32];
  c3_y          cry_y[32];
  c3_y          sym_y[32];   //  symmetric key
  u3_ship       spo_u;       //  sponsor
  c3_o          spo_o;       //  sponsor known
  //  route
  c3_o          lan_o;       //  have a lane
  c3_y          hop_y;
  mesa_lane     lan_u;
  c3_d          con_d;       //  last contact (ms)
  c3_o          liv_o;       //  qos %live
  //  system flows (eng_sys.c)
  c3_o          gfo_o;       //  gall /gf flow open
  c3_d          gfb_d;       //    its bone
  c3_o          jao_o;       //  jael /public-keys flow open (they're a parent)
  c3_d          jab_d;       //    its bone
  c3_o          moa_o;       //  moon: asked our parent for our keys
  c3_o          gbo_o;       //  their gall /gf flow to us (for our flubs)
  c3_d          gbb_d;       //    its bone
  //  classic ames (eng_ames.c)
  c3_o          aho_o;       //  we've asked them to move to mesa
  c3_d          ahb_d;       //    on this bone
  c3_d          ahd_d;       //    last asked (ms)
  c3_d          ahk_d;       //    last acked (ms)
  //  ossuary
  c3_d          bon_d;       //  next-bone
  eng_flow_map  flo_u;
  eng_crk       crk_u[4];    //  corked backward flows
  c3_d          use_d;       //  last looked up (ms), for unloading
  //  .pit
  eng_req*      req_u;       //  in order made
  eng_req*      ret_u;       //    the last
  eng_req_map   rem_u;       //    by path
  //  congestion (vere's u3_gage)
  c3_d          rtt_d, rtv_d, rto_d, wnd_d, wnf_d, sst_d;
  c3_d          fly_d;       //  in the window: fragments, first requests
  c3_d          snd_d;       //  requests sent (send numbers)
  c3_d          rec_d;       //  loss recovery: losses of requests sent
                             //  before this were already counted
  //  pokes heard before we had their keys, to replay once we do
  eng_out*      stl_u;
  c3_w          stn_w;
};

#define NAME     eng_peer_map
#define KEY_TY   u3_ship_key
#define VAL_TY   eng_peer*
#define HASH_FN  eng_ship_hash
#define CMPR_FN  eng_ship_cmpr
typedef struct { c3_d d[2]; } u3_ship_key;
static inline uint64_t eng_ship_hash(u3_ship_key k) {
  uint64_t h = k.d[0] ^ (k.d[1] * 0x9e3779b97f4a7c15ull);
  h ^= h >> 23; h *= 0x2127599bf4325c37ull; h ^= h >> 47;
  return h;
}
static inline bool eng_ship_cmpr(u3_ship_key a, u3_ship_key b) {
  return a.d[0] == b.d[0] && a.d[1] == b.d[1];
}
#include "verstable.h"

/* served-message cache line: one message in our namespace
*/
typedef struct _eng_line {
  c3_c*             pat_c;   //  namespace path, no leading '/'
  c3_d              tob_d;
  c3_y*             ser_y;   //  message bytes
  u3_auth_data      aut_u;   //  sig or hmac over the root
  c3_d              tof_d;
  lss_hash*         pof_u;   //  proof, lss_proof_size(tof) hashes
  c3_d              pon_d;   //  proof hashes
  lss_pair*         par_u;   //  pairs, one per leaf
  c3_y*             pap_y;   //  per leaf: has pair
  struct _eng_line* nex_u;   //  eviction order
  struct _eng_line* pre_u;
} eng_line;

#define NAME     eng_line_map
#define KEY_TY   c3_c*         //  the line's pat_c
#define VAL_TY   eng_line*
#include "verstable.h"

/* shut key: one of our group keys for a %shut namespace (+sy-plug)
*/
typedef struct _eng_shut {
  c3_d              kid_d;
  c3_y              key_y[32];
  c3_c*             pre_c;   //  the paths it covers (case not compared)
  struct _eng_shut* nex_u;
} eng_shut;

/* responder pit: lanes waiting for a page of ours (vere's pit)
*/
typedef struct _eng_wait {
  c3_c*             pat_c;
  mesa_lane         lan_u;
  c3_d              exp_d;
  struct _eng_wait* nex_u;
} eng_wait;

struct mesa_engine {
  mesa_hooks    hok_u;
  mesa_store*   sto_u;
  //  identity
  u3_ship       our_u;
  c3_d          rif_d;
  c3_d          lyf_d;
  mesa_keys     key_u;
  c3_y          pas_y[65];
  u3_ship       spo_u;       //  our sponsor
  u3_ship       gal_u;       //  our galaxy
  c3_o          fak_o;       //  fake network
  //  time
  c3_d          now_d;
  //  state
  eng_peer_map  per_u;
  c3_w          ali_w;       //  peers without keys
  const c3_y*   hea_y;       //  the datagram we're hearing, if any
  c3_z          hea_z;
  eng_line*     lin_u;       //  served-message cache, oldest first
  eng_line*     lit_u;       //    newest
  eng_line_map  lim_u;       //    by path
  c3_d          lis_d;       //  cache bytes
  eng_wait*     wat_u;
  eng_shut*     shu_u;       //  our %shut keys, by kid ascending
  //  ping
  c3_o          pin_o;
  c3_d          pin_d;       //  next ping, ms
  c3_d          pib_d;       //  ping bone
  c3_o          pif_o;       //  ping in flight
  //  transactions
  c3_w          txn_w;       //  nesting depth
  c3_o          app_o;       //  app-level transaction open
  c3_o          bad_o;       //  store failed; engine stopped
  c3_o          lod_o;       //  loading from the store: no lazy loads
  c3_d          gcd_d;       //  next sweep for idle peers, ms
  c3_o          rel_o;       //  reload memory from the store after commit
  c3_d          api_d;       //  mutating API calls, for nack rollback
  c3_o          hok_o;       //  inside a hook
  eng_out*      out_u;       //  queued datagrams
  eng_out**     ont_u;       //  tail
};

/* eng_util.c
*/
void      eng_send(mesa_engine* eng, mesa_lane lan_u, c3_y* buf_y, c3_z len_z);
void      eng_send_pact(mesa_engine* eng, mesa_lane lan_u, u3_mesa_pact* pac_u);
void      eng_trace(const c3_c* dir_c, u3_mesa_pact* pac_u);
c3_o      eng_lanes_eq(mesa_lane a, mesa_lane b);
c3_c*     eng_path_text(mesa_noun pax);      //  "/a/b" from a path noun
mesa_noun eng_path_noun(const c3_c* pat_c);  //  path noun from "/a/b"
c3_c*     eng_ship_text(u3_ship who_u);      //  "~zod" (static buffer)

void      eng_reload(mesa_engine* eng);
void      eng_out_cut(mesa_engine* eng, eng_out** mar_u);
c3_o      eng_txn_open(mesa_engine* eng);
c3_o      eng_txn_close(mesa_engine* eng);   //  commit; c3n on failure
void      eng_txn_fail(mesa_engine* eng);

void      eng_put(mesa_engine* eng, c3_y* key_y, c3_z key_z, mesa_noun val);
void      eng_del(mesa_engine* eng, c3_y* key_y, c3_z key_z);

/* eng_peer.c
*/
eng_peer* eng_peer_get(mesa_engine* eng, u3_ship who_u);
eng_peer* eng_peer_new(mesa_engine* eng, u3_ship who_u);
eng_peer* eng_peer_make(mesa_engine* eng, u3_ship who_u);  //  get or new
c3_o      eng_peer_evict(mesa_engine* eng);
void      eng_peer_save(mesa_engine* eng, eng_peer* per_u);
void      eng_peer_ask(mesa_engine* eng, eng_peer* per_u);
c3_o      eng_peer_galaxy(mesa_engine* eng, eng_peer* per_u, c3_y* gal_y);
c3_o      eng_ship_galaxy(mesa_engine* eng, u3_ship who_u, c3_y* gal_y);
c3_w      eng_peer_lanes(mesa_engine* eng, eng_peer* per_u, mesa_lane las_u[2]);
void      eng_peer_heard(mesa_engine* eng, eng_peer* per_u,
                         mesa_lane lan_u, c3_y hop_y, mesa_lane* nex_u);
void      eng_peer_set_keys(mesa_engine* eng, eng_peer* per_u,
                            c3_d rif_d, c3_d lyf_d, const c3_y pas_y[65]);
void      eng_peer_breach(mesa_engine* eng, eng_peer* per_u);
void      eng_peer_free(eng_peer* per_u);
void      eng_peer_sweep(mesa_engine* eng);
eng_peer* eng_peer_load(mesa_engine* eng, u3_ship who_u);   //  engine.c
void      eng_del_prefix(mesa_engine* eng, const c3_y* pre_y, c3_z pre_z);
void      eng_peer_known(mesa_engine* eng, eng_peer* per_u);
void      eng_comet_proof(mesa_engine* eng, eng_peer* per_u, mesa_lane lan_u);
c3_o      eng_comet_take(mesa_engine* eng, eng_peer* per_u,
                         const c3_c* pat_c, const c3_y roo_y[32],
                         u3_auth_data* aut_u, mesa_noun gag);

/* eng_space.c: namespace paths and crypto
*/
c3_c*     eng_chum_path(mesa_engine* eng, eng_peer* per_u, c3_o our_o,
                        const c3_c* inn_c);
c3_c*     eng_publ_path(c3_d lyf_d, const c3_c* inn_c);
c3_o      eng_auth_page(mesa_engine* eng, eng_peer* per_u, const c3_c* pat_c,
                        const c3_y roo_y[32], u3_auth_data* aut_u);
c3_y*     eng_open_page(mesa_engine* eng, eng_peer* per_u, const c3_c* pat_c,
                        c3_z len_z, const c3_y* dat_y, c3_z* out_z);
c3_c*     eng_open_chum(mesa_engine* eng, eng_peer* per_u, const c3_c* pat_c);
c3_c*     eng_shut_path(c3_d kid_d, const c3_y key_y[32], const c3_c* inn_c);
c3_y*     eng_open_shut(const c3_y key_y[32], const c3_c* pat_c,
                        c3_z len_z, const c3_y* dat_y, c3_z* out_z);
c3_y*     eng_shut_parse(const c3_c* pat_c, c3_d* kid_d, c3_z* cyf_z);
eng_shut* eng_shut_get(mesa_engine* eng, c3_d kid_d);
c3_o      eng_shut_under(const c3_c* pre_c, const c3_c* inn_c);
c3_o      eng_shut_covered(mesa_engine* eng, const c3_c* inn_c);
void      eng_shut_free(mesa_engine* eng);

/* eng_serve.c
*/
eng_line* eng_line_get(mesa_engine* eng, const c3_c* pat_c);
void      eng_serve_peek(mesa_engine* eng, mesa_lane lan_u, u3_mesa_name* nam_u);
c3_o      eng_page_first(mesa_engine* eng, eng_line* lin_u,
                         u3_mesa_data* dat_u, c3_y** fre_y);
void      eng_wait_add(mesa_engine* eng, const c3_c* pat_c, mesa_lane lan_u);
void      eng_give_page(mesa_engine* eng, eng_peer* per_u, const c3_c* pat_c);
void      eng_cache_drop(mesa_engine* eng, const c3_c* pat_c);
void      eng_cache_free(mesa_engine* eng);

/* eng_fetch.c
*/
eng_req*  eng_req_find(eng_peer* per_u, const c3_c* pat_c);
eng_req*  eng_req_add(mesa_engine* eng, eng_peer* per_u, const c3_c* pat_c,
                      eng_req_e typ_e);
void      eng_req_del(eng_peer* per_u, eng_req* req_u);
void      eng_req_rename(eng_peer* per_u, eng_req* req_u, c3_c* pat_c);
void      eng_req_clear(eng_peer* per_u);
void      eng_req_free(eng_req* req_u);
void      eng_req_send(mesa_engine* eng, eng_peer* per_u, eng_req* req_u);
void      eng_hear_page(mesa_engine* eng, mesa_lane lan_u, u3_mesa_pact* pac_u);
void      eng_req_wake(mesa_engine* eng, eng_peer* per_u);
void      eng_req_rebase(mesa_engine* eng, eng_peer* per_u);
c3_c*     eng_keen_path(eng_peer* per_u, eng_req* req_u);
c3_d      eng_req_deadline(eng_peer* per_u);

/* eng_flow.c
*/
eng_flow* eng_flow_get(eng_peer* per_u, c3_d bon_d, eng_dire dir_e);
eng_flow* eng_flow_make(mesa_engine* eng, eng_peer* per_u, c3_d bon_d,
                        eng_dire dir_e);
void      eng_flow_save(mesa_engine* eng, eng_peer* per_u, eng_flow* flo_u);
void      eng_flow_free(eng_flow* flo_u);
c3_o      eng_flow_corked(eng_peer* per_u, c3_d bon_d, eng_dire dir_e);
void      eng_cork_put(mesa_engine* eng, eng_peer* per_u, c3_d bon_d,
                       c3_o sav_o);
void      eng_cork_load(eng_peer* per_u, c3_d lo_d, c3_d hi_d);
c3_w      eng_cork_count(eng_peer* per_u);
void      eng_cork_wipe(mesa_engine* eng, eng_peer* per_u);
c3_c*     eng_flow_path(c3_d bon_d, const c3_c* lod_c, eng_dire dir_e,
                        u3_ship srv_u, c3_d seq_d);
void      eng_hear_pact(mesa_engine* eng, mesa_lane from, const c3_y* buf,
                        c3_z len);
void      eng_peer_stash(mesa_engine* eng, eng_peer* per_u, mesa_lane lan_u);
void      eng_flow_append(eng_flow* flo_u, eng_msg* msg_u);
void      eng_flow_pump(mesa_engine* eng, eng_peer* per_u, eng_flow* flo_u,
                        mesa_noun msg);
void      eng_flow_send(mesa_engine* eng, eng_peer* per_u, eng_flow* flo_u);
mesa_noun eng_flow_peek(mesa_engine* eng, eng_peer* per_u, c3_d bon_d,
                        eng_dire dir_e, const c3_c* lod_c, c3_d seq_d);
void      eng_hear_poke(mesa_engine* eng, mesa_lane lan_u, u3_mesa_pact* pac_u);
void      eng_flow_sink(mesa_engine* eng, eng_peer* per_u, c3_d bon_d,
                        eng_dire dir_e, c3_d seq_d, mesa_noun gag);
void      eng_flow_take(mesa_engine* eng, eng_peer* per_u, eng_req* req_u,
                        mesa_noun gag);
void      eng_flow_done(mesa_engine* eng, eng_peer* per_u, eng_flow* flo_u,
                        mesa_noun err);
void      eng_flow_resume(mesa_engine* eng, eng_peer* per_u);
void      eng_flow_halt(mesa_engine* eng, eng_peer* per_u, eng_flow* flo_u);
void      eng_flow_unhalt(mesa_engine* eng, eng_peer* per_u, eng_flow* flo_u);

/* eng_ames.c: classic ames, to move peers to mesa
*/
void      eng_ames_hear(mesa_engine* eng, mesa_lane lan_u,
                        const c3_y* buf_y, c3_z len_z);
void      eng_ames_ahoy(mesa_engine* eng, eng_peer* per_u, mesa_lane lan_u);

/* eng_sys.c: system flows (gall /gf /gk, jael /public-keys), points
*/
c3_o      eng_sys_bone(eng_peer* per_u, c3_d bon_d);
void      eng_sys_track(mesa_engine* eng, eng_peer* per_u, mesa_noun msg);
c3_o      eng_sys_moon(mesa_engine* eng, eng_peer* per_u);
c3_o      eng_sys_plea(mesa_engine* eng, eng_peer* per_u, eng_flow* flo_u,
                       mesa_noun van, mesa_noun pax, mesa_noun pay);
c3_o      eng_sys_boon(mesa_engine* eng, eng_peer* per_u, c3_d bon_d,
                       mesa_noun pay);
c3_o      eng_sys_done(mesa_engine* eng, eng_peer* per_u, c3_d bon_d,
                       mesa_noun err);
void      eng_sys_breach(mesa_engine* eng, eng_peer* per_u);
c3_o      eng_sys_flub(mesa_engine* eng, eng_peer* per_u, eng_flow* flo_u,
                       mesa_noun van, mesa_noun pax);
void      eng_sys_spur(mesa_engine* eng, const c3_c* dap_c);
void      eng_point_apply(mesa_engine* eng, const mesa_point* pot);

/* keys for the store
*/
#define KEY_IDENT 'I'
#define KEY_PEER  'P'
#define KEY_FLOW  'F'
#define KEY_LOAD  'L'
#define KEY_CORK  'C'
#define KEY_GALL  'G'    //  their /gf bone, for telling them our flubs
#define KEY_ROUT  'R'    //  a route, kept when a peer leaves memory: [ip port]
#define KEY_SHUT  'S'    //  a %shut key of ours: 'S' kid(8) -> [key prefix]

c3_z eng_key(c3_y typ_y, u3_ship who_u, c3_d bon_d, eng_dire dir_e,
             c3_d seq_d, c3_y key_y[40]);

#endif /* ifndef MESA_ENG_H */

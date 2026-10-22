/// @file
///
/// Classic ames, just enough to move peers to mesa.
///
/// A ship that knew us before mesa (or learned our keys before hearing
/// from us, as a planet does its own moons) keeps talking to us in classic
/// ames packets, and ignores our mesa packets. Arvo's migration is led by
/// an %ahoy $plea over classic ames: the receiver moves all its flows with
/// us to mesa (+migrate-peer in ames.hoon) and acks. So when we hear a
/// classic packet from a ship whose keys we have, we send it an %ahoy as
/// classic message 1 on a fresh bone, and keep doing so while it still
/// speaks classic ames to us. Once it has migrated, it resends whatever it
/// had for us over mesa, and our own messages get through.
///
/// The packet format is +etch-shot/+sift-shot (lull.hoon) around an
/// AES-256-SIV encrypted +shut-packet (+etch-shut-packet, ames.hoon).

#include "eng.h"
#include "aes_siv.h"
#include "sha.h"

#define AMES_VERSION   0        //  +protocol-version
#define AMES_AHOY_MS   5000     //  at most one %ahoy per peer this often
#define AMES_HUSH_MS   120000   //  after an ack, the classic packets still in
                                //  flight aren't news: don't ahoy again

static c3_y
_ship_size(u3_ship who_u)
{
  c3_y buf_y[16];
  c3_z met_z;

  u3_ship_to_bytes(who_u, 16, buf_y);
  met_z = mesa_met3_bytes(16, buf_y);
  return ( met_z <= 2 ) ? 2 : ( met_z <= 4 ) ? 4 : ( met_z <= 8 ) ? 8 : 16;
}

static c3_y
_ship_rank(c3_y siz_y)
{
  return ( 2 == siz_y ) ? 0 : ( 4 == siz_y ) ? 1 : ( 8 == siz_y ) ? 2 : 3;
}

/* _sivc_key(): +shaz of the symmetric key
*/
static void
_sivc_key(eng_peer* per_u, c3_y key_y[64])
{
  mesa_shal(mesa_met3_bytes(32, per_u->sym_y), per_u->sym_y, key_y);
}

/* _sivc_vec(): ~[sndr rcvr sndr-life rcvr-life] as atom bytes
*/
typedef struct {
  c3_y sndr_y[16], rcvr_y[16], sl_y[8], rl_y[8];
  const c3_y* ads_y[4];
  c3_z        adz_z[4];
} _vec;

static void
_vec_make(_vec* vec_u, u3_ship snd_u, u3_ship rcv_u, c3_d sly_d, c3_d rly_d)
{
  u3_ship_to_bytes(snd_u, 16, vec_u->sndr_y);
  u3_ship_to_bytes(rcv_u, 16, vec_u->rcvr_y);
  for ( c3_w i = 0; i < 8; i++ ) {
    vec_u->sl_y[i] = (c3_y)(sly_d >> (8 * i));
    vec_u->rl_y[i] = (c3_y)(rly_d >> (8 * i));
  }
  vec_u->ads_y[0] = vec_u->sndr_y;  vec_u->adz_z[0] = mesa_met3_bytes(16, vec_u->sndr_y);
  vec_u->ads_y[1] = vec_u->rcvr_y;  vec_u->adz_z[1] = mesa_met3_bytes(16, vec_u->rcvr_y);
  vec_u->ads_y[2] = vec_u->sl_y;    vec_u->adz_z[2] = mesa_met3_bytes(8, vec_u->sl_y);
  vec_u->ads_y[3] = vec_u->rl_y;    vec_u->adz_z[3] = mesa_met3_bytes(8, vec_u->rl_y);
}

static c3_w
_mug_bytes(c3_z len_z, const c3_y* buf_y)
{
  mesa_noun atm = mesa_atom_bytes(len_z, buf_y);
  c3_w      mug_w = mesa_mug(atm);
  mesa_unref(atm);
  return mug_w;
}

/* _etch(): +etch-shot of a shut-packet to [her]; the packet's length
*/
static c3_z
_etch(mesa_engine* eng, eng_peer* per_u, mesa_noun shu, c3_o req_o, c3_y* out_y)
{
  c3_y   key_y[64];
  _vec   vec_u;
  c3_z   msg_z, bod_z, out_z;
  c3_y*  msg_y = mesa_jam_bytes(shu, &msg_z);
  c3_y   sns_y = _ship_size(eng->our_u);
  c3_y   rcs_y = _ship_size(per_u->who_u);
  c3_y*  bod_y;
  c3_y*  con_y;
  c3_w   hed_w, chk_w;

  //  content: siv (16) ++ len (2) ++ cyf
  //
  _sivc_key(per_u, key_y);
  _vec_make(&vec_u, eng->our_u, per_u->who_u, eng->lyf_d, per_u->lyf_d);
  bod_z = 1 + sns_y + rcs_y + 18 + msg_z;
  bod_y = c3_calloc(bod_z);
  con_y = bod_y + 1 + sns_y + rcs_y;
  mesa_sivc_en(key_y, 4, vec_u.ads_y, vec_u.adz_z, msg_y, msg_z, con_y, con_y + 18);
  con_y[16] = (c3_y)(msg_z & 0xff);
  con_y[17] = (c3_y)((msg_z >> 8) & 0xff);
  c3_free(msg_y);

  //  body: ticks, sndr, rcvr, content
  //
  bod_y[0] = (c3_y)((eng->lyf_d & 0xf) | ((per_u->lyf_d & 0xf) << 4));
  {
    c3_y buf_y[16];
    u3_ship_to_bytes(eng->our_u, 16, buf_y);
    memcpy(bod_y + 1, buf_y, sns_y);
    u3_ship_to_bytes(per_u->who_u, 16, buf_y);
    memcpy(bod_y + 1 + sns_y, buf_y, rcs_y);
  }
  bod_z = mesa_met3_bytes(bod_z, bod_y);     //  an atom: no trailing zeros
  chk_w = _mug_bytes(bod_z, bod_y) & 0xfffff;

  //  header: [2 0] [1 req] [1 sam] [3 version] [2 rank] [2 rank]
  //          [20 checksum] [1 relayed]   (loobeans: & is 0)
  //
  hed_w = ((c3_w)(( c3y == req_o ) ? 0 : 1) << 2)
        | ((c3_w)0 << 3)
        | ((c3_w)AMES_VERSION << 4)
        | ((c3_w)_ship_rank(sns_y) << 7)
        | ((c3_w)_ship_rank(rcs_y) << 9)
        | (chk_w << 11)
        | ((c3_w)1 << 31);                   //  no origin: .?(~) is %.n

  out_y[0] = (c3_y)hed_w;
  out_y[1] = (c3_y)(hed_w >> 8);
  out_y[2] = (c3_y)(hed_w >> 16);
  out_y[3] = (c3_y)(hed_w >> 24);
  memcpy(out_y + 4, bod_y, bod_z);
  out_z = 4 + bod_z;
  c3_free(bod_y);
  return out_z;
}

static c3_y*
_copy(const c3_y* buf_y, c3_z len_z)
{
  c3_y* out_y = c3_malloc(len_z);
  memcpy(out_y, buf_y, len_z);
  return out_y;
}

/* eng_ames_ahoy(): ask [per_u] to move to mesa
*/
void
eng_ames_ahoy(mesa_engine* eng, eng_peer* per_u, mesa_lane lan_u)
{
  c3_y      buf_y[PACT_SIZE];
  c3_z      len_z;
  mesa_noun ple, shu;
  c3_y*     ple_y;
  c3_z      ple_z;
  mesa_lane las_u[2];
  c3_w      lan_w;

  if (  (c3y == per_u->aho_o)
     && (  (eng->now_d < per_u->ahd_d + AMES_AHOY_MS)
        || (per_u->ahk_d && (eng->now_d < per_u->ahk_d + AMES_HUSH_MS)) ) )
  {
    return;
  }

  //  a fresh bone, never to carry anything else: once they migrate, it's
  //  a mesa flow on their side whose message 1 is done. It's saved, so
  //  that every %ahoy we ever send them is the same message (one they
  //  can ack again after migrating)
  //
  if ( c3n == per_u->aho_o ) {
    per_u->ahb_d = per_u->bon_d;
    per_u->bon_d += 4;
    per_u->aho_o = c3y;
    eng_peer_save(eng, per_u);
  }
  per_u->ahd_d = eng->now_d;

  //  [%$ /mesa-2 %ahoy ~], jammed: a one-fragment message
  //
  ple = mesa_trel(mesa_atom_u64(0),
                  mesa_cell(mesa_atom_cord("mesa-2"), mesa_atom_u64(0)),
                  mesa_cell(mesa_atom_cord("ahoy"), mesa_atom_u64(0)));
  ple_y = mesa_jam_bytes(ple, &ple_z);
  mesa_unref(ple);

  //  [bone message-num=1 %& num-fragments=1 fragment-num=0 fragment],
  //  with the bone's last bit swizzled, as every sender does
  //
  shu = mesa_trel(mesa_atom_u64(per_u->ahb_d ^ 1), mesa_atom_u64(1),
                  mesa_cell(mesa_atom_u64(0),
                            mesa_trel(mesa_atom_u64(1), mesa_atom_u64(0),
                                      mesa_atom_bytes(ple_z, ple_y))));
  c3_free(ple_y);
  len_z = _etch(eng, per_u, shu, c3y, buf_y);
  mesa_unref(shu);

  mesa_log("mesa: %s talks classic ames to us; asking it to move to mesa",
           eng_ship_text(per_u->who_u));

  //  back the way it came, and by the usual routes (eng_send() takes
  //  its buffer)
  //
  eng_send(eng, lan_u, _copy(buf_y, len_z), len_z);
  lan_w = eng_peer_lanes(eng, per_u, las_u);
  for ( c3_w i = 0; i < lan_w; i++ ) {
    if ( c3n == eng_lanes_eq(las_u[i], lan_u) ) {
      eng_send(eng, las_u[i], _copy(buf_y, len_z), len_z);
    }
  }
}

/* _sift(): what's in a classic packet from [per_u], for the log
*/
static void
_sift(mesa_engine* eng, eng_peer* per_u, const c3_y* con_y, c3_z con_z)
{
  c3_y      key_y[64];
  _vec      vec_u;
  c3_z      len_z;
  c3_y*     cyf_y;
  c3_y*     msg_y;
  mesa_noun shu;

  if ( con_z < 18 ) {
    return;
  }
  len_z = (c3_z)con_y[16] | ((c3_z)con_y[17] << 8);
  if ( (con_z - 18) > len_z ) {
    return;
  }
  cyf_y = c3_calloc(len_z ? len_z : 1);
  msg_y = c3_calloc(len_z ? len_z : 1);
  memcpy(cyf_y, con_y + 18, con_z - 18);

  _sivc_key(per_u, key_y);
  _vec_make(&vec_u, per_u->who_u, eng->our_u, per_u->lyf_d, eng->lyf_d);
  if ( c3y == mesa_sivc_de(key_y, 4, vec_u.ads_y, vec_u.adz_z, con_y, cyf_y,
                           len_z, msg_y) )
  {
    c3_d bon_d = 0, num_d = 0, tag_d = 1, sub_d = 1;

    shu = mesa_cue_bytes(len_z, msg_y);
    if ( MESA_NONE != shu ) {
      mesa_atom_get_u64(mesa_at(2, shu), &bon_d);
      mesa_atom_get_u64(mesa_at(6, shu), &num_d);
      mesa_atom_get_u64(mesa_at(14, shu), &tag_d);
      if ( 1 == tag_d ) {
        mesa_atom_get_u64(mesa_at(30, shu), &sub_d);
      }
      //  (their ack swizzles our swizzled bone back)
      //
      if ( (1 == tag_d) && (1 == sub_d) && (bon_d == per_u->ahb_d)
         && (c3y == per_u->aho_o) )
      {
        //  [%| %| ok=? lag=@dr]: a loobean, %.y is 0
        //
        c3_d ok_d = 1;
        mesa_atom_get_u64(mesa_at(62, shu), &ok_d);
        if ( 0 == ok_d ) {
          if ( !per_u->ahk_d || (eng->now_d >= per_u->ahk_d + AMES_HUSH_MS) ) {
            mesa_log("mesa: %s acked our %%ahoy: it speaks mesa to us now",
                     eng_ship_text(per_u->who_u));
          }
          per_u->ahk_d = eng->now_d;
        }
        else {
          mesa_log("mesa: %s nacked our %%ahoy: it can't move to mesa yet",
                   eng_ship_text(per_u->who_u));
        }
      }
      else if ( getenv("MESA_TRACE") ) {
        c3_c txt_c[200] = "";
        c3_d nfr_d = 0, fra_d = 0;

        //  a whole one-fragment message: show what it is
        //
        if (  (0 == tag_d)
           && mesa_atom_get_u64(mesa_at(30, shu), &nfr_d) && (1 == nfr_d)
           && mesa_atom_get_u64(mesa_at(62, shu), &fra_d) && (0 == fra_d)
           && mesa_is_atom(mesa_at(63, shu)) )
        {
          mesa_noun fag = mesa_at(63, shu);
          c3_z      fan_z = mesa_met3(fag);
          c3_y*     fay_y = c3_malloc(fan_z ? fan_z : 1);
          mesa_noun msg;

          mesa_atom_get_bytes(fag, fan_z, fay_y);
          msg = mesa_cue_bytes(fan_z, fay_y);
          c3_free(fay_y);
          if ( MESA_NONE != msg ) {
            size_t    n = 0;
            mesa_noun cur = msg;
            for ( c3_w i = 0; i < 6 && n < 180; i++ ) {
              mesa_noun hed = mesa_is_cell(cur) ? mesa_head(cur) : cur;
              if ( mesa_is_atom(hed) && mesa_met3(hed) && mesa_met3(hed) < 24 ) {
                c3_c* c = mesa_atom_get_cord(hed);
                n += (size_t)snprintf(txt_c + n, sizeof(txt_c) - n, " %s", c);
                c3_free(c);
              }
              else if ( mesa_is_atom(hed) ) {
                n += (size_t)snprintf(txt_c + n, sizeof(txt_c) - n, " %zu-byte", mesa_met3(hed));
              }
              else {
                n += (size_t)snprintf(txt_c + n, sizeof(txt_c) - n, " [...]");
              }
              if ( !mesa_is_cell(cur) ) break;
              cur = mesa_tail(cur);
            }
            mesa_unref(msg);
          }
        }
        mesa_log("mesa:   classic %s bone %" PRIu64 " message %" PRIu64 ": %s%s",
                 eng_ship_text(per_u->who_u), bon_d, num_d,
                 ( 0 == tag_d ) ? "fragment"
                 : ( 0 == sub_d ) ? "fragment ack" : "message ack", txt_c);
      }
      mesa_unref(shu);
    }
  }
  c3_free(cyf_y);
  c3_free(msg_y);
}

/* eng_ames_hear(): a classic ames packet
*/
void
eng_ames_hear(mesa_engine* eng, mesa_lane lan_u, const c3_y* buf_y, c3_z len_z)
{
  c3_w       hed_w;
  c3_y       sns_y, rcs_y, ver_y;
  const c3_y* bod_y;
  c3_z       bod_z;
  u3_ship    snd_u, rcv_u;
  eng_peer*  per_u;
  c3_y       tmp_y[16];

  if ( len_z < 4 ) {
    return;
  }
  hed_w = (c3_w)buf_y[0] | ((c3_w)buf_y[1] << 8)
        | ((c3_w)buf_y[2] << 16) | ((c3_w)buf_y[3] << 24);
  ver_y = (hed_w >> 4) & 0x7;
  sns_y = (c3_y)(2 << ((hed_w >> 7) & 0x3));
  rcs_y = (c3_y)(2 << ((hed_w >> 9) & 0x3));
  if ( (0 != (hed_w & 0x3)) || (AMES_VERSION != ver_y) ) {
    return;
  }

  //  relayed packets carry the origin lane first (+sift-shot)
  //
  bod_y = buf_y + 4;
  bod_z = len_z - 4;
  if ( !((hed_w >> 31) & 1) ) {
    if ( bod_z < 6 ) return;
    bod_y += 6;
    bod_z -= 6;
  }
  if (  (bod_z < (c3_z)(1 + sns_y + rcs_y))
     || ((c3_w)(_mug_bytes(bod_z, bod_y) & 0xfffff) != ((hed_w >> 11) & 0xfffff)) )
  {
    return;
  }

  memset(tmp_y, 0, 16);
  memcpy(tmp_y, bod_y + 1, sns_y);
  u3_ship_of_bytes(snd_u, 16, tmp_y);
  memset(tmp_y, 0, 16);
  memcpy(tmp_y, bod_y + 1 + sns_y, rcs_y);
  u3_ship_of_bytes(rcv_u, 16, tmp_y);

  if (  (c3n == u3_ships_equal(rcv_u, eng->our_u))
     || (c3y == u3_ships_equal(snd_u, eng->our_u))
     || (_ship_size(snd_u) != sns_y)
     || (c3__pawn == u3_ship_rank(snd_u)) )   //  comets' open packets: not yet
  {
    return;
  }

  if ( !(per_u = eng_peer_get(eng, snd_u)) ) {
    if ( (eng->ali_w >= ENG_ALIEN_MAX) && (c3n == eng_peer_evict(eng)) ) {
      return;
    }
    per_u = eng_peer_new(eng, snd_u);
  }
  if ( c3n == per_u->kno_o ) {
    eng_peer_ask(eng, per_u);           //  they'll send again
    return;
  }
  //  their life and ours, as they know them
  //
  if (  ((bod_y[0] & 0xf) != (per_u->lyf_d & 0xf))
     || (((bod_y[0] >> 4) & 0xf) != (eng->lyf_d & 0xf)) )
  {
    return;
  }

  _sift(eng, per_u, bod_y + 1 + sns_y + rcs_y, bod_z - (1 + sns_y + rcs_y));
  eng_ames_ahoy(eng, per_u, lan_u);
}

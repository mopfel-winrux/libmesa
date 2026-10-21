/// @file

#include "compat.h"
#include "crypto.h"
#include "sha.h"
#include "ship.h"
#include "mesa/ident.h"
#include "mesa/noun.h"

/* ships
*/
static void
_ship_in(u3_ship who_u, mesa_ship who)
{
  who_u[0] = who.lo;
  who_u[1] = who.hi;
}

static mesa_ship
_ship_out(u3_ship who_u)
{
  return (mesa_ship){ .lo = who_u[0], .hi = who_u[1] };
}

bool
mesa_ship_parse(const char* str, mesa_ship* who)
{
  u3_ship who_u;

  if ( c3n == u3_ship_of_string(who_u, str) ) {
    return false;
  }
  *who = _ship_out(who_u);
  return true;
}

char*
mesa_ship_render(mesa_ship who)
{
  u3_ship who_u;
  _ship_in(who_u, who);
  return u3_ship_to_string(who_u);
}

mesa_rank
mesa_ship_rank(mesa_ship who)
{
  if      ( who.hi )       return MESA_PAWN;
  else if ( who.lo >> 32 ) return MESA_EARL;
  else if ( who.lo >> 16 ) return MESA_DUKE;
  else if ( who.lo >> 8 )  return MESA_KING;
  else                     return MESA_CZAR;
}

mesa_ship
mesa_ship_sein(mesa_ship who)
{
  u3_ship who_u, par_u;
  _ship_in(who_u, who);
  u3_ship_sein(par_u, who_u);
  return _ship_out(par_u);
}

bool
mesa_ship_eq(mesa_ship a, mesa_ship b)
{
  return (a.lo == b.lo) && (a.hi == b.hi);
}

static void
_ship_bytes(mesa_ship who, c3_y buf_y[16])
{
  for ( c3_w i = 0; i < 8; i++ ) {
    buf_y[i]     = (c3_y)(who.lo >> (8 * i));
    buf_y[8 + i] = (c3_y)(who.hi >> (8 * i));
  }
}

static mesa_ship
_ship_of_bytes(const c3_y buf_y[16])
{
  mesa_ship who = {0};

  for ( c3_w i = 0; i < 8; i++ ) {
    who.lo |= (c3_d)buf_y[i]     << (8 * i);
    who.hi |= (c3_d)buf_y[8 + i] << (8 * i);
  }
  return who;
}

/* keys
*/
void
mesa_ident_pass(const mesa_ident* ide, uint8_t pass[65])
{
  mesa_keys key_u;

  mesa_keys_from_ring(&key_u, 65, ide->ring);
  mesa_keys_pass(&key_u, pass);
}

/* _pit(): +pit:nu:cric for suite %b, the ring from [w] bits of a seed
*/
static void
_pit(c3_w wid_w, c3_z len_z, const c3_y* sed_y, c3_y rin_y[65])
{
  c3_z  byt_z = (wid_w / 8) + ((wid_w % 8) ? 1 : 0);
  c3_y* buf_y = c3_calloc(c3_max(byt_z, 1));

  //  (shal wid seed) hashes the low [wid] bytes of the seed atom
  //
  memcpy(buf_y, sed_y, c3_min(len_z, byt_z));
  rin_y[0] = 'B';
  mesa_shal(byt_z, buf_y, rin_y + 1);
  c3_free(buf_y);
}

/* _fig(): +fig:ex:cric, (shaf %bfig pass) as a ship
*/
static mesa_ship
_fig(const c3_y pas_y[65])
{
  c3_y fig_y[16];

  mesa_shaf(4, (const c3_y*)"bfig", 65, pas_y, fig_y);
  return _ship_of_bytes(fig_y);
}

void
mesa_ident_fake(mesa_ship who, mesa_ident* out)
{
  c3_y sed_y[16];

  _ship_bytes(who, sed_y);
  memset(out, 0, sizeof(*out));
  out->who  = who;
  out->rift = 0;
  out->life = 1;
  _pit(512, 16, sed_y, out->ring);
}

/* comets
*/
static bool
_is_star(mesa_ship who)
{
  return MESA_KING == mesa_ship_rank(who);
}

bool
mesa_comet_check(mesa_ship who, const uint8_t pass[65])
{
  if ( MESA_PAWN != mesa_ship_rank(who) ) {
    return false;
  }
  if ( !_is_star(mesa_ship_sein(who)) ) {
    return false;
  }
  if ( 'b' != pass[0] ) {
    return false;
  }
  return mesa_ship_eq(who, _fig(pass));
}

bool
mesa_comet_mine(size_t len,
                const mesa_ship* stars,
                const uint8_t eny[64],
                uint64_t max_tries,
                mesa_ident* out)
{
  c3_y* set_y = c3_calloc(0x10000 / 8);
  c3_y  sed_y[64];
  bool  fon = false;

  for ( size_t i = 0; i < len; i++ ) {
    if ( !_is_star(stars[i]) ) {
      c3_free(set_y);
      return false;
    }
    set_y[stars[i].lo >> 3] |= (c3_y)(1 << (stars[i].lo & 7));
  }

  memcpy(sed_y, eny, 64);

  for ( uint64_t t = 0; t < max_tries; t++ ) {
    mesa_ident ide = {0};
    mesa_keys  key_u;
    c3_y       pas_y[65];
    mesa_ship  who;

    _pit(512, 64, sed_y, ide.ring);
    mesa_keys_from_ring(&key_u, 65, ide.ring);
    mesa_keys_pass(&key_u, pas_y);
    who = _fig(pas_y);

    //  disallow 64-bit or smaller addresses; parent must be listed
    //
    if ( (MESA_PAWN == mesa_ship_rank(who))
      && (set_y[(who.lo & 0xffff) >> 3] & (1 << (who.lo & 7))) )
    {
      ide.who  = who;
      ide.rift = 0;
      ide.life = 1;
      *out = ide;
      fon = true;
      break;
    }

    //  eny = +(eny)
    //
    for ( c3_w i = 0; i < 64; i++ ) {
      if ( ++sed_y[i] ) break;
    }
  }

  c3_free(set_y);
  return fon;
}

/* @uw
*/
static c3_ys
_uw_digit(c3_c c)
{
  if ( (c >= '0') && (c <= '9') ) return c - '0';
  if ( (c >= 'a') && (c <= 'z') ) return 10 + (c - 'a');
  if ( (c >= 'A') && (c <= 'Z') ) return 36 + (c - 'A');
  if ( '-' == c ) return 62;
  if ( '~' == c ) return 63;
  return -1;
}

static const c3_c _uw_chars[] =
  "0123456789abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ-~";

/* _uw_parse(): @uw text to little-endian bytes (malloc'd); NULL if bad.
**
**   Whitespace and the '.' separators are skipped.
*/
static c3_y*
_uw_parse(const c3_c* txt_c, c3_z* len_z)
{
  c3_z  dig_z = 0;
  c3_z  cap_z;
  c3_y* buf_y;
  c3_z  bit_z = 0;

  while ( *txt_c && isspace((c3_y)*txt_c) ) txt_c++;
  if ( ('0' != txt_c[0]) || ('w' != txt_c[1]) ) {
    return 0;
  }
  txt_c += 2;

  for ( const c3_c* c = txt_c; *c; c++ ) {
    if ( _uw_digit(*c) >= 0 ) {
      dig_z++;
    }
    else if ( ('.' != *c) && !isspace((c3_y)*c) ) {
      return 0;
    }
  }
  if ( 0 == dig_z ) {
    return 0;
  }

  cap_z = ((dig_z * 6) + 7) / 8;
  buf_y = c3_calloc(cap_z);

  //  least significant digit last
  //
  for ( c3_z i = strlen(txt_c); i-- > 0; ) {
    c3_ys dig = _uw_digit(txt_c[i]);

    if ( dig < 0 ) continue;

    for ( c3_w b = 0; b < 6; b++, bit_z++ ) {
      if ( dig & (1 << b) ) {
        buf_y[bit_z >> 3] |= (c3_y)(1 << (bit_z & 7));
      }
    }
  }

  *len_z = cap_z;
  return buf_y;
}

/* _uw_render(): little-endian bytes as @uw text (scot %uw), malloc'd
*/
static c3_c*
_uw_render(c3_z len_z, const c3_y* buf_y)
{
  c3_z  bit_z = mesa_met3_bytes(len_z, buf_y) * 8;
  c3_z  dig_z = (bit_z + 5) / 6;
  c3_c* out_c;
  c3_c* tmp_c;
  c3_z  n_z = 0;

  //  trim leading zero digits
  //
  while ( dig_z ) {
    c3_y dig = 0;
    for ( c3_w b = 0; b < 6; b++ ) {
      c3_z i_z = ((dig_z - 1) * 6) + b;
      if ( (i_z < len_z * 8) && (buf_y[i_z >> 3] & (1 << (i_z & 7))) ) {
        dig |= (1 << b);
      }
    }
    if ( dig ) break;
    dig_z--;
  }

  if ( 0 == dig_z ) {
    return strdup("0w0");
  }

  tmp_c = c3_malloc(dig_z + (dig_z / 5) + 1);

  //  emit least significant first, then reverse
  //
  for ( c3_z d = 0; d < dig_z; d++ ) {
    c3_y dig = 0;

    if ( d && (0 == (d % 5)) ) {
      tmp_c[n_z++] = '.';
    }
    for ( c3_w b = 0; b < 6; b++ ) {
      c3_z i_z = (d * 6) + b;
      if ( (i_z < len_z * 8) && (buf_y[i_z >> 3] & (1 << (i_z & 7))) ) {
        dig |= (1 << b);
      }
    }
    tmp_c[n_z++] = _uw_chars[dig];
  }

  out_c = c3_malloc(n_z + 3);
  out_c[0] = '0';
  out_c[1] = 'w';
  for ( c3_z i = 0; i < n_z; i++ ) {
    out_c[2 + i] = tmp_c[n_z - 1 - i];
  }
  out_c[2 + n_z] = 0;

  c3_free(tmp_c);
  return out_c;
}

/* keyfiles
*/
static bool
_atom_ship(mesa_noun som, mesa_ship* who)
{
  c3_y buf_y[16];

  if ( !mesa_is_atom(som) || (mesa_met3(som) > 16) ) {
    return false;
  }
  mesa_atom_get_bytes(som, 16, buf_y);
  *who = _ship_of_bytes(buf_y);
  return true;
}

static bool
_atom_u64(mesa_noun som, uint64_t* out)
{
  return mesa_is_atom(som) && mesa_atom_get_u64(som, out);
}

/* _ring(): a suite %b ring atom
*/
static bool
_ring(mesa_noun som, c3_y rin_y[65])
{
  if ( !mesa_is_atom(som) || (mesa_met3(som) > 65) ) {
    return false;
  }
  mesa_atom_get_bytes(som, 65, rin_y);
  return 'B' == rin_y[0];
}

int
mesa_keyfile_parse(const char* text, mesa_ident* out, size_t cap)
{
  c3_z      len_z;
  c3_y*     buf_y = _uw_parse(text, &len_z);
  mesa_noun fed;
  int       num_i = -1;

  if ( !buf_y ) {
    return -1;
  }

  fed = mesa_cue_bytes(len_z, buf_y);
  c3_free(buf_y);

  if ( MESA_NONE == fed || !mesa_is_cell(fed) ) {
    goto done;
  }

  //  +seed:jael: [who=ship lyf=life key=ring sig=(unit oath:pki)]
  //
  if ( mesa_is_atom(mesa_head(fed)) ) {
    mesa_ident ide = { .rift = MESA_RIFT_UNKNOWN };

    if (  !_atom_ship(mesa_at(2, fed), &ide.who)
       || !_atom_u64(mesa_at(6, fed), &ide.life)
       || !_ring(mesa_at(14, fed), ide.ring) )
    {
      goto done;
    }
    if ( cap ) {
      out[0] = ide;
    }
    num_i = 1;
    goto done;
  }

  //  +feed:jael: [[%1 ~] who kyz] or [[%2 ~] who ryf kyz]
  //
  {
    uint64_t  tag, rif = MESA_RIFT_UNKNOWN;
    mesa_ship who;
    mesa_noun kyz;
    c3_z      n_z = 0;

    if (  !_atom_u64(mesa_at(4, fed), &tag)
       || !mesa_is_atom(mesa_at(5, fed))
       || (0 != mesa_met3(mesa_at(5, fed)))
       || !_atom_ship(mesa_at(6, fed), &who) )
    {
      goto done;
    }

    if ( 1 == tag ) {
      kyz = mesa_at(7, fed);
    }
    else if ( 2 == tag ) {
      if ( !_atom_u64(mesa_at(14, fed), &rif) ) goto done;
      kyz = mesa_at(15, fed);
    }
    else goto done;

    while ( (MESA_NONE != kyz) && mesa_is_cell(kyz) ) {
      mesa_ident ide = { .who = who, .rift = rif };
      mesa_noun  i   = mesa_head(kyz);

      if (  !mesa_is_cell(i)
         || !_atom_u64(mesa_head(i), &ide.life)
         || !_ring(mesa_tail(i), ide.ring) )
      {
        goto done;
      }
      if ( n_z < cap ) {
        out[n_z] = ide;
      }
      n_z++;
      kyz = mesa_tail(kyz);
    }

    if ( (MESA_NONE == kyz) || (0 != mesa_met3(kyz)) || !n_z ) {
      goto done;
    }
    num_i = (int)n_z;
  }

done:
  if ( MESA_NONE != fed ) {
    mesa_unref(fed);
  }
  return num_i;
}

char*
mesa_keyfile_render(const mesa_ident* ide)
{
  c3_y      who_y[16];
  mesa_noun fed;
  c3_y*     jam_y;
  c3_z      jam_z;
  c3_c*     out_c;

  _ship_bytes(ide->who, who_y);

  //  [[%2 ~] who ryf [[lyf key] ~]]
  //
  fed = mesa_qual(mesa_cell(mesa_atom_u64(2), mesa_atom_u64(0)),
                  mesa_atom_bytes(16, who_y),
                  mesa_atom_u64(( MESA_RIFT_UNKNOWN == ide->rift )
                                ? 0 : ide->rift),
                  mesa_cell(mesa_cell(mesa_atom_u64(ide->life),
                                      mesa_atom_bytes(65, ide->ring)),
                            mesa_atom_u64(0)));

  jam_y = mesa_jam_bytes(fed, &jam_z);
  out_c = _uw_render(jam_z, jam_y);

  c3_free(jam_y);
  mesa_unref(fed);
  return out_c;
}

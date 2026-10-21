/// @file
///
/// Standalone nouns. Representation (mesa_noun is a 64-bit word):
///
///   ...0   direct atom, value = som >> 1 (< 2^63)
///   ..01   pointer to an _atom_box (value >= 2^63, normalized)
///   ..11   pointer to a _cell_box
///
/// Every traversal (unref, mug, eq, jam, cue) uses an explicit stack, so
/// deeply nested nouns (e.g. long lists) can't overflow the C stack.

#include "compat.h"
#include "mesa/noun.h"
#include "murmur3.h"

typedef struct {
  c3_h  rc_h;   //  refcount
  c3_h  mug_h;  //  cached +mug, 0 if not yet computed
  c3_d  haz_d;  //  cached 64-bit hash (jam dedup), 0 if not yet computed
  c3_z  len_z;  //  bytes in buf_y; buf_y[len_z - 1] != 0
  c3_y  buf_y[];
} _atom_box;

typedef struct {
  c3_h      rc_h;
  c3_h      mug_h;
  c3_d      haz_d;
  mesa_noun hed;
  mesa_noun tel;
} _cell_box;

#define _is_dir(som)   (0 == ((som) & 1))
#define _is_ind(som)   (1 == ((som) & 3))
#define _is_cel(som)   (3 == ((som) & 3))
#define _to_atom(som)  ((_atom_box*)(uintptr_t)((som) & ~(c3_d)3))
#define _to_cell(som)  ((_cell_box*)(uintptr_t)((som) & ~(c3_d)3))

/* _stack: growable stack of fixed-size frames.
*/
typedef struct {
  c3_y* buf_y;
  c3_z  siz_z;   //  frame size
  c3_z  len_z;   //  frames in use
  c3_z  cap_z;   //  frames allocated
} _stack;

static void
_stack_init(_stack* sac_u, c3_z siz_z)
{
  sac_u->siz_z = siz_z;
  sac_u->len_z = 0;
  sac_u->cap_z = 64;
  sac_u->buf_y = c3_malloc(siz_z * sac_u->cap_z);
}

static void*
_stack_push(_stack* sac_u)
{
  if ( sac_u->len_z == sac_u->cap_z ) {
    sac_u->cap_z *= 2;
    sac_u->buf_y = c3_realloc(sac_u->buf_y, sac_u->siz_z * sac_u->cap_z);
  }
  return sac_u->buf_y + (sac_u->siz_z * sac_u->len_z++);
}

static inline void*
_stack_peek(_stack* sac_u)
{
  return sac_u->buf_y + (sac_u->siz_z * (sac_u->len_z - 1));
}

static inline void
_stack_pop(_stack* sac_u)
{
  sac_u->len_z--;
}

static void
_stack_free(_stack* sac_u)
{
  c3_free(sac_u->buf_y);
}

/* refcounts
*/
mesa_noun
mesa_ref(mesa_noun som)
{
  u3_assert( MESA_NONE != som );

  if ( !_is_dir(som) ) {
    c3_h* rc_h = &_to_cell(som)->rc_h;   //  rc_h is first in both boxes
    u3_assert( UINT32_MAX != *rc_h );
    (*rc_h)++;
  }
  return som;
}

void
mesa_unref(mesa_noun som)
{
  _stack sac_u;

  if ( (MESA_NONE == som) || _is_dir(som) ) {
    return;
  }

  _stack_init(&sac_u, sizeof(mesa_noun));
  *(mesa_noun*)_stack_push(&sac_u) = som;

  while ( sac_u.len_z ) {
    mesa_noun cur = *(mesa_noun*)_stack_peek(&sac_u);
    _stack_pop(&sac_u);

    if ( _is_dir(cur) ) {
      continue;
    }
    else if ( _is_ind(cur) ) {
      _atom_box* box_u = _to_atom(cur);
      if ( 0 == --box_u->rc_h ) {
        c3_free(box_u);
      }
    }
    else {
      _cell_box* box_u = _to_cell(cur);
      if ( 0 == --box_u->rc_h ) {
        *(mesa_noun*)_stack_push(&sac_u) = box_u->hed;
        *(mesa_noun*)_stack_push(&sac_u) = box_u->tel;
        c3_free(box_u);
      }
    }
  }

  _stack_free(&sac_u);
}

/* atoms
*/
mesa_noun
mesa_atom_u64(c3_d val_d)
{
  if ( val_d >> 63 ) {
    _atom_box* box_u = c3_malloc(sizeof(*box_u) + 8);
    box_u->rc_h  = 1;
    box_u->mug_h = 0;
    box_u->haz_d = 0;
    box_u->len_z = 8;
    c3_etch_chub(box_u->buf_y, val_d);
    return (mesa_noun)(uintptr_t)box_u | 1;
  }
  return val_d << 1;
}

mesa_noun
mesa_atom_bytes(c3_z len_z, const c3_y* buf_y)
{
  while ( len_z && (0 == buf_y[len_z - 1]) ) {
    len_z--;
  }

  if ( len_z <= 8 ) {
    c3_y byt_y[8] = {0};
    memcpy(byt_y, buf_y, len_z);
    return mesa_atom_u64(c3_sift_chub(byt_y));
  }
  else {
    _atom_box* box_u = c3_malloc(sizeof(*box_u) + len_z);
    box_u->rc_h  = 1;
    box_u->mug_h = 0;
    box_u->haz_d = 0;
    box_u->len_z = len_z;
    memcpy(box_u->buf_y, buf_y, len_z);
    return (mesa_noun)(uintptr_t)box_u | 1;
  }
}

mesa_noun
mesa_atom_cord(const c3_c* str_c)
{
  return mesa_atom_bytes(strlen(str_c), (const c3_y*)str_c);
}

bool
mesa_is_atom(mesa_noun som)
{
  u3_assert( MESA_NONE != som );
  return !_is_cel(som);
}

bool
mesa_is_cell(mesa_noun som)
{
  u3_assert( MESA_NONE != som );
  return _is_cel(som);
}

/* _atom_view(): borrow an atom as a little-endian byte buffer.
*/
static const c3_y*
_atom_view(mesa_noun som, c3_y tmp_y[8], c3_z* len_z)
{
  u3_assert( !_is_cel(som) );

  if ( _is_dir(som) ) {
    c3_d val_d = som >> 1;
    c3_etch_chub(tmp_y, val_d);
    *len_z = (c3_bits_chub(val_d) + 7) >> 3;
    return tmp_y;
  }
  else {
    _atom_box* box_u = _to_atom(som);
    *len_z = box_u->len_z;
    return box_u->buf_y;
  }
}

c3_z
mesa_met3(mesa_noun som)
{
  c3_y tmp_y[8];
  c3_z len_z;
  _atom_view(som, tmp_y, &len_z);
  return len_z;
}

c3_z
mesa_met0(mesa_noun som)
{
  c3_y        tmp_y[8];
  c3_z        len_z;
  const c3_y* buf_y = _atom_view(som, tmp_y, &len_z);

  if ( !len_z ) {
    return 0;
  }
  return ((len_z - 1) << 3) + c3_bits_half(buf_y[len_z - 1]);
}

void
mesa_atom_get_bytes(mesa_noun som, c3_z len_z, c3_y* out_y)
{
  c3_y        tmp_y[8];
  c3_z        met_z;
  const c3_y* buf_y = _atom_view(som, tmp_y, &met_z);
  c3_z        cop_z = c3_min(len_z, met_z);

  memcpy(out_y, buf_y, cop_z);
  memset(out_y + cop_z, 0, len_z - cop_z);
}

bool
mesa_atom_get_u64(mesa_noun som, c3_d* out_d)
{
  c3_y byt_y[8];

  if ( 8 < mesa_met3(som) ) {
    return false;
  }
  mesa_atom_get_bytes(som, 8, byt_y);
  *out_d = c3_sift_chub(byt_y);
  return true;
}

c3_c*
mesa_atom_get_cord(mesa_noun som)
{
  c3_z  len_z = mesa_met3(som);
  c3_c* str_c = c3_malloc(len_z + 1);

  mesa_atom_get_bytes(som, len_z, (c3_y*)str_c);
  str_c[len_z] = 0;
  return str_c;
}

/* cells
*/
mesa_noun
mesa_cell(mesa_noun hed, mesa_noun tel)
{
  _cell_box* box_u;

  u3_assert( (MESA_NONE != hed) && (MESA_NONE != tel) );

  box_u = c3_malloc(sizeof(*box_u));
  box_u->rc_h  = 1;
  box_u->mug_h = 0;
  box_u->haz_d = 0;
  box_u->hed   = hed;
  box_u->tel   = tel;
  return (mesa_noun)(uintptr_t)box_u | 3;
}

mesa_noun
mesa_trel(mesa_noun a, mesa_noun b, mesa_noun c)
{
  return mesa_cell(a, mesa_cell(b, c));
}

mesa_noun
mesa_qual(mesa_noun a, mesa_noun b, mesa_noun c, mesa_noun d)
{
  return mesa_cell(a, mesa_cell(b, mesa_cell(c, d)));
}

mesa_noun
mesa_head(mesa_noun som)
{
  u3_assert( (MESA_NONE != som) && _is_cel(som) );
  return _to_cell(som)->hed;
}

mesa_noun
mesa_tail(mesa_noun som)
{
  u3_assert( (MESA_NONE != som) && _is_cel(som) );
  return _to_cell(som)->tel;
}

mesa_noun
mesa_at(c3_d axe_d, mesa_noun som)
{
  c3_y dep_y;

  if ( 0 == axe_d ) {
    return MESA_NONE;
  }

  //  walk the bits of the axis below the leading 1, high to low
  //
  dep_y = c3_bits_chub(axe_d) - 1;

  while ( dep_y-- ) {
    if ( !_is_cel(som) ) {
      return MESA_NONE;
    }
    som = ( (axe_d >> dep_y) & 1 )
        ? _to_cell(som)->tel
        : _to_cell(som)->hed;
  }

  return som;
}

/* +mug
*/
static c3_h
_mug_both(c3_h lef_h, c3_h rit_h)
{
  c3_y len_y = 4 + ((c3_bits_half(rit_h) + 0x7) >> 3);
  c3_h syd_h = 0xdeadbeef;
  c3_h   i_h = 0;
  c3_y buf_y[8];

  c3_etch_half(buf_y, lef_h);
  c3_etch_half(buf_y + 4, rit_h);

  while ( i_h < 8 ) {
    c3_h haz_h;
    c3_h ham_h;

    MurmurHash3_x86_32(buf_y, len_y, syd_h, &haz_h);
    ham_h = (haz_h >> 31) ^ (haz_h & 0x7fffffff);

    if ( 0 == ham_h ) {
      syd_h++; i_h++;
    }
    else {
      return ham_h;
    }
  }

  return 0xfffe;
}

static c3_h
_mug_atom(mesa_noun som)
{
  c3_y        tmp_y[8];
  c3_z        len_z;
  const c3_y* buf_y;

  if ( _is_ind(som) && _to_atom(som)->mug_h ) {
    return _to_atom(som)->mug_h;
  }

  buf_y = _atom_view(som, tmp_y, &len_z);
  u3_assert( UINT32_MAX >= len_z );

  {
    c3_h mug_h = mesa_mug_bytes(buf_y, (c3_h)len_z);
    if ( _is_ind(som) ) {
      _to_atom(som)->mug_h = mug_h;
    }
    return mug_h;
  }
}

static inline c3_o
_mug_ready(mesa_noun som)
{
  return __(!_is_cel(som) || _to_cell(som)->mug_h);
}

static inline c3_h
_mug_ready_get(mesa_noun som)
{
  return _is_cel(som) ? _to_cell(som)->mug_h : _mug_atom(som);
}

c3_h
mesa_mug(mesa_noun som)
{
  _stack sac_u;

  u3_assert( MESA_NONE != som );

  if ( c3y == _mug_ready(som) ) {
    return _mug_ready_get(som);
  }

  _stack_init(&sac_u, sizeof(mesa_noun));
  *(mesa_noun*)_stack_push(&sac_u) = som;

  while ( sac_u.len_z ) {
    mesa_noun  cur   = *(mesa_noun*)_stack_peek(&sac_u);
    _cell_box* cel_u = _to_cell(cur);

    if ( cel_u->mug_h ) {
      _stack_pop(&sac_u);
    }
    else if ( c3n == _mug_ready(cel_u->hed) ) {
      *(mesa_noun*)_stack_push(&sac_u) = cel_u->hed;
    }
    else if ( c3n == _mug_ready(cel_u->tel) ) {
      *(mesa_noun*)_stack_push(&sac_u) = cel_u->tel;
    }
    else {
      cel_u->mug_h = _mug_both(_mug_ready_get(cel_u->hed),
                               _mug_ready_get(cel_u->tel));
      _stack_pop(&sac_u);
    }
  }

  _stack_free(&sac_u);
  return _to_cell(som)->mug_h;
}

/* equality
*/
static c3_o
_eq_atom(mesa_noun a, mesa_noun b)
{
  c3_y        tma_y[8], tmb_y[8];
  c3_z        lea_z, leb_z;
  const c3_y* bua_y = _atom_view(a, tma_y, &lea_z);
  const c3_y* bub_y = _atom_view(b, tmb_y, &leb_z);

  return __((lea_z == leb_z) && (0 == memcmp(bua_y, bub_y, lea_z)));
}

bool
mesa_eq(mesa_noun a, mesa_noun b)
{
  _stack sac_u;
  c3_o   ret_o = c3y;

  u3_assert( (MESA_NONE != a) && (MESA_NONE != b) );

  _stack_init(&sac_u, 2 * sizeof(mesa_noun));
  {
    mesa_noun* fam = _stack_push(&sac_u);
    fam[0] = a;
    fam[1] = b;
  }

  while ( (c3y == ret_o) && sac_u.len_z ) {
    mesa_noun* fam = _stack_peek(&sac_u);
    mesa_noun  x = fam[0], y = fam[1];
    _stack_pop(&sac_u);

    if ( x == y ) {
      continue;
    }
    if ( _is_cel(x) != _is_cel(y) ) {
      ret_o = c3n;
    }
    else if ( !_is_cel(x) ) {
      ret_o = _eq_atom(x, y);
    }
    else {
      _cell_box* xel_u = _to_cell(x);
      _cell_box* yel_u = _to_cell(y);

      if (  (xel_u->mug_h && yel_u->mug_h && (xel_u->mug_h != yel_u->mug_h))
         || (xel_u->haz_d && yel_u->haz_d && (xel_u->haz_d != yel_u->haz_d)) )
      {
        ret_o = c3n;
      }
      else {
        mesa_noun* fam_t = _stack_push(&sac_u);
        fam_t[0] = xel_u->tel;
        fam_t[1] = yel_u->tel;

        mesa_noun* fam_h = _stack_push(&sac_u);
        fam_h[0] = xel_u->hed;
        fam_h[1] = yel_u->hed;
      }
    }
  }

  _stack_free(&sac_u);
  return _(ret_o);
}

/* 64-bit structural hash, for jam's dedup table.
**
**   +mug is only 31 bits, and iterating it up a deep spine falls into a
**   cycle after ~2^15.5 levels; distinct subtrees then share a mug and
**   jam's dedup degenerates into deep equality checks. jam's output
**   doesn't depend on the hash, so use a wider one.
*/
static c3_d
_haz_mix(c3_d hed_d, c3_d tel_d)
{
  c3_d haz_d = hed_d ^ ((tel_d << 31) | (tel_d >> 33)) ^ 0x9e3779b97f4a7c15ULL;
  haz_d ^= haz_d >> 30;
  haz_d *= 0xbf58476d1ce4e5b9ULL;
  haz_d ^= haz_d >> 27;
  haz_d *= 0x94d049bb133111ebULL;
  haz_d ^= haz_d >> 31;
  return haz_d ? haz_d : 1;
}

static c3_d
_haz_atom(mesa_noun som)
{
  c3_y        tmp_y[8];
  c3_z        len_z;
  const c3_y* buf_y;
  c3_d        out_d[2];

  if ( _is_ind(som) && _to_atom(som)->haz_d ) {
    return _to_atom(som)->haz_d;
  }

  buf_y = _atom_view(som, tmp_y, &len_z);
  u3_assert( INT32_MAX >= len_z );
  MurmurHash3_x64_128(buf_y, (c3_i)len_z, 0x6d657361, out_d);
  out_d[0] = out_d[0] ? out_d[0] : 1;

  if ( _is_ind(som) ) {
    _to_atom(som)->haz_d = out_d[0];
  }
  return out_d[0];
}

static inline c3_o
_haz_ready(mesa_noun som)
{
  return __(!_is_cel(som) || _to_cell(som)->haz_d);
}

static inline c3_d
_haz_ready_get(mesa_noun som)
{
  return _is_cel(som) ? _to_cell(som)->haz_d : _haz_atom(som);
}

static c3_d
_haz(mesa_noun som)
{
  _stack sac_u;

  if ( c3y == _haz_ready(som) ) {
    return _haz_ready_get(som);
  }

  _stack_init(&sac_u, sizeof(mesa_noun));
  *(mesa_noun*)_stack_push(&sac_u) = som;

  while ( sac_u.len_z ) {
    mesa_noun  cur   = *(mesa_noun*)_stack_peek(&sac_u);
    _cell_box* cel_u = _to_cell(cur);

    if ( cel_u->haz_d ) {
      _stack_pop(&sac_u);
    }
    else if ( c3n == _haz_ready(cel_u->hed) ) {
      *(mesa_noun*)_stack_push(&sac_u) = cel_u->hed;
    }
    else if ( c3n == _haz_ready(cel_u->tel) ) {
      *(mesa_noun*)_stack_push(&sac_u) = cel_u->tel;
    }
    else {
      cel_u->haz_d = _haz_mix(_haz_ready_get(cel_u->hed),
                              _haz_ready_get(cel_u->tel));
      _stack_pop(&sac_u);
    }
  }

  _stack_free(&sac_u);
  return _to_cell(som)->haz_d;
}

/* noun -> bit position, for jam backreferences (keys are borrowed)
*/
static uint64_t
_jam_hash(mesa_noun som)
{
  return _haz(som);
}

static bool
_jam_cmpr(mesa_noun a, mesa_noun b)
{
  return mesa_eq(a, b);
}

#define NAME     _jam_map
#define KEY_TY   mesa_noun
#define VAL_TY   c3_d
#define HASH_FN  _jam_hash
#define CMPR_FN  _jam_cmpr
#include "verstable.h"

/* bit position -> noun, for cue backreferences (values are owned)
*/
static uint64_t
_cue_hash(c3_d pos_d)
{
  pos_d ^= pos_d >> 33;
  pos_d *= 0xff51afd7ed558ccdULL;
  pos_d ^= pos_d >> 33;
  return pos_d;
}

static bool
_cue_cmpr(c3_d a_d, c3_d b_d)
{
  return a_d == b_d;
}

#define NAME     _cue_map
#define KEY_TY   c3_d
#define VAL_TY   mesa_noun
#define HASH_FN  _cue_hash
#define CMPR_FN  _cue_cmpr
#include "verstable.h"

/* bit writer
*/
typedef struct {
  c3_y* buf_y;
  c3_z  cap_z;   //  bytes allocated
  c3_d  pos_d;   //  bits written
} _bits_w;

static void
_bits_grow(_bits_w* wri_u, c3_d bit_d)
{
  c3_z ned_z = (wri_u->pos_d + bit_d + 7) >> 3;

  if ( ned_z > wri_u->cap_z ) {
    c3_z cap_z = c3_max(ned_z, 2 * wri_u->cap_z);
    wri_u->buf_y = c3_realloc(wri_u->buf_y, cap_z);
    memset(wri_u->buf_y + wri_u->cap_z, 0, cap_z - wri_u->cap_z);
    wri_u->cap_z = cap_z;
  }
}

/* _bits_put(): append the low [bit_y] (<= 64) bits of [val_d]
*/
static void
_bits_put(_bits_w* wri_u, c3_y bit_y, c3_d val_d)
{
  _bits_grow(wri_u, bit_y);

  while ( bit_y ) {
    c3_d pos_d = wri_u->pos_d;
    c3_y off_y = pos_d & 7;
    c3_y can_y = c3_min(8 - off_y, bit_y);
    c3_y msk_y = (c3_y)((1U << can_y) - 1);

    wri_u->buf_y[pos_d >> 3] |= (c3_y)((val_d & msk_y) << off_y);

    val_d >>= can_y;
    bit_y  -= can_y;
    wri_u->pos_d += can_y;
  }
}

/* _bits_put_zeros(): append [bit_d] zero bits
*/
static void
_bits_put_zeros(_bits_w* wri_u, c3_d bit_d)
{
  _bits_grow(wri_u, bit_d);
  wri_u->pos_d += bit_d;
}

/* _bits_put_atom(): append the low [bit_d] bits of [som]
*/
static void
_bits_put_atom(_bits_w* wri_u, c3_d bit_d, mesa_noun som)
{
  c3_y        tmp_y[8];
  c3_z        len_z;
  const c3_y* buf_y = _atom_view(som, tmp_y, &len_z);
  c3_z        i_z   = 0;

  _bits_grow(wri_u, bit_d);

  while ( bit_d ) {
    c3_y can_y = (c3_y)c3_min(8, bit_d);
    c3_y byt_y = ( i_z < len_z ) ? buf_y[i_z] : 0;
    _bits_put(wri_u, can_y, byt_y);
    bit_d -= can_y;
    i_z++;
  }
}

/* _mat_put(): +mat [som] (length-encode)
*/
static void
_mat_put(_bits_w* wri_u, mesa_noun som)
{
  c3_d b_d = mesa_met0(som);

  if ( 0 == b_d ) {
    _bits_put(wri_u, 1, 1);
  }
  else {
    c3_y c_y = c3_bits_chub(b_d);

    _bits_put_zeros(wri_u, c_y);
    _bits_put(wri_u, 1, 1);
    _bits_put(wri_u, c_y - 1, b_d & ((c_y > 1) ? ((1ULL << (c_y - 1)) - 1) : 0));
    _bits_put_atom(wri_u, b_d, som);
  }
}

static void
_mat_put_d(_bits_w* wri_u, c3_d val_d)
{
  mesa_noun som = mesa_atom_u64(val_d);
  _mat_put(wri_u, som);
  mesa_unref(som);
}

static c3_y*
_jam_buf(mesa_noun som, c3_z* len_z)
{
  _bits_w  wri_u = { .buf_y = c3_calloc(16), .cap_z = 16, .pos_d = 0 };
  _stack   sac_u;
  _jam_map map_u;

  _jam_map_init(&map_u);
  _stack_init(&sac_u, sizeof(mesa_noun));
  *(mesa_noun*)_stack_push(&sac_u) = som;

  while ( sac_u.len_z ) {
    mesa_noun     cur = *(mesa_noun*)_stack_peek(&sac_u);
    _jam_map_itr  itr = _jam_map_get(&map_u, cur);
    c3_d          pos_d = wri_u.pos_d;

    _stack_pop(&sac_u);

    if ( !_jam_map_is_end(itr) ) {
      c3_d bak_d = itr.data->val;

      if ( !_is_cel(cur) && (mesa_met0(cur) <= (c3_d)c3_bits_chub(bak_d)) ) {
        _bits_put(&wri_u, 1, 0);
        _mat_put(&wri_u, cur);
      }
      else {
        _bits_put(&wri_u, 2, 3);
        _mat_put_d(&wri_u, bak_d);
      }
    }
    else {
      if ( _jam_map_is_end(_jam_map_insert(&map_u, cur, pos_d)) ) {
        u3_assert(!"jam: out of memory");
      }

      if ( !_is_cel(cur) ) {
        _bits_put(&wri_u, 1, 0);
        _mat_put(&wri_u, cur);
      }
      else {
        _bits_put(&wri_u, 2, 1);
        *(mesa_noun*)_stack_push(&sac_u) = _to_cell(cur)->tel;
        *(mesa_noun*)_stack_push(&sac_u) = _to_cell(cur)->hed;
      }
    }
  }

  _stack_free(&sac_u);
  _jam_map_cleanup(&map_u);

  //  trim to the significant bytes of the resulting atom
  //
  {
    c3_z len = (wri_u.pos_d + 7) >> 3;
    while ( len && (0 == wri_u.buf_y[len - 1]) ) {
      len--;
    }
    *len_z = len;
  }

  return wri_u.buf_y;
}

c3_y*
mesa_jam_bytes(mesa_noun som, c3_z* len_z)
{
  u3_assert( MESA_NONE != som );
  return _jam_buf(som, len_z);
}

mesa_noun
mesa_jam(mesa_noun som)
{
  c3_z      len_z;
  c3_y*     buf_y = mesa_jam_bytes(som, &len_z);
  mesa_noun pro   = mesa_atom_bytes(len_z, buf_y);

  c3_free(buf_y);
  return pro;
}

/* bit reader
*/
typedef struct {
  const c3_y* buf_y;
  c3_d        len_d;   //  bits available
} _bits_r;

static inline c3_y
_bits_bit(_bits_r* red_u, c3_d pos_d)
{
  return (red_u->buf_y[pos_d >> 3] >> (pos_d & 7)) & 1;
}

/* _bits_get(): read [bit_y] (<= 64) bits at [pos_d]; caller bounds-checks
*/
static c3_d
_bits_get(_bits_r* red_u, c3_d pos_d, c3_y bit_y)
{
  c3_d val_d = 0;
  c3_y got_y = 0;

  while ( got_y < bit_y ) {
    c3_y off_y = (pos_d + got_y) & 7;
    c3_y can_y = c3_min(8 - off_y, bit_y - got_y);
    c3_y byt_y = red_u->buf_y[(pos_d + got_y) >> 3] >> off_y;

    val_d |= (c3_d)(byt_y & ((1U << can_y) - 1)) << got_y;
    got_y += can_y;
  }

  return val_d;
}

/* _bits_get_atom(): read [bit_d] bits at [pos_d] as an atom
*/
static mesa_noun
_bits_get_atom(_bits_r* red_u, c3_d pos_d, c3_d bit_d)
{
  c3_z      len_z = (bit_d + 7) >> 3;
  c3_y*     buf_y = c3_malloc(c3_max(len_z, 1));
  mesa_noun pro;

  for ( c3_z i_z = 0; i_z < len_z; i_z++ ) {
    c3_y can_y = (c3_y)c3_min(8, bit_d - (i_z << 3));
    buf_y[i_z] = (c3_y)_bits_get(red_u, pos_d + (i_z << 3), can_y);
  }

  pro = mesa_atom_bytes(len_z, buf_y);
  c3_free(buf_y);
  return pro;
}

/* _rub_get(): +rub at [pos_d]; produce bits consumed, or 0 on failure
*/
static c3_d
_rub_get(_bits_r* red_u, c3_d pos_d, mesa_noun* out)
{
  c3_d c_d = 0;

  while ( 1 ) {
    if ( pos_d + c_d >= red_u->len_d ) {
      return 0;
    }
    if ( _bits_bit(red_u, pos_d + c_d) ) {
      break;
    }
    c_d++;
  }

  if ( 0 == c_d ) {
    *out = 0;
    return 1;
  }

  //  a length-of-length over 64 bits can't describe a real buffer
  //
  if ( c_d > 64 ) {
    return 0;
  }

  {
    c3_d d_d = pos_d + c_d + 1;
    c3_d e_d;

    if ( d_d + (c_d - 1) > red_u->len_d ) {
      return 0;
    }

    e_d = (1ULL << (c_d - 1)) + _bits_get(red_u, d_d, (c3_y)(c_d - 1));

    if (  (e_d > red_u->len_d)
       || (d_d + (c_d - 1) + e_d > red_u->len_d) )
    {
      return 0;
    }

    *out = _bits_get_atom(red_u, d_d + (c_d - 1), e_d);
    return (2 * c_d) + e_d;
  }
}

typedef struct {
  c3_d      pos_d;   //  bit position of this cell
  mesa_noun hed;     //  MESA_NONE until the head is decoded
} _cue_frame;

mesa_noun
mesa_cue_bytes(c3_z len_z, const c3_y* buf_y)
{
  _bits_r  red_u = { .buf_y = buf_y, .len_d = (c3_d)len_z << 3 };
  _stack   sac_u;
  _cue_map map_u;
  c3_d     pos_d = 0;
  mesa_noun pro  = MESA_NONE;

  if ( (len_z >> 61) ) {
    return MESA_NONE;
  }

  _cue_map_init(&map_u);
  _stack_init(&sac_u, sizeof(_cue_frame));

  while ( 1 ) {
    mesa_noun cur;
    c3_d      sat_d = pos_d;

    if ( pos_d >= red_u.len_d ) {
      goto fail;
    }

    if ( 0 == _bits_bit(&red_u, pos_d) ) {
      c3_d use_d = _rub_get(&red_u, pos_d + 1, &cur);
      if ( !use_d ) {
        goto fail;
      }
      pos_d += 1 + use_d;
    }
    else {
      if ( pos_d + 1 >= red_u.len_d ) {
        goto fail;
      }

      if ( 0 == _bits_bit(&red_u, pos_d + 1) ) {
        _cue_frame* fam_u = _stack_push(&sac_u);
        fam_u->pos_d = sat_d;
        fam_u->hed   = MESA_NONE;
        pos_d += 2;
        continue;
      }
      else {
        mesa_noun     bak;
        c3_d          bak_d;
        _cue_map_itr  itr;
        c3_d          use_d = _rub_get(&red_u, pos_d + 2, &bak);

        if ( !use_d ) {
          goto fail;
        }
        if ( !mesa_atom_get_u64(bak, &bak_d) ) {
          mesa_unref(bak);
          goto fail;
        }
        mesa_unref(bak);

        itr = _cue_map_get(&map_u, bak_d);
        if ( _cue_map_is_end(itr) ) {
          goto fail;
        }
        cur = mesa_ref(itr.data->val);
        pos_d += 2 + use_d;
      }
    }

    //  backreferences to atoms are themselves recorded only by position
    //  of the original; record fresh atoms here
    //
    if ( !_is_cel(cur) && (0 == _bits_bit(&red_u, sat_d)) ) {
      if ( _cue_map_is_end(_cue_map_insert(&map_u, sat_d, mesa_ref(cur))) ) {
        u3_assert(!"cue: out of memory");
      }
    }

    //  unwind completed cells
    //
    while ( 1 ) {
      _cue_frame* fam_u;

      if ( 0 == sac_u.len_z ) {
        pro = cur;
        goto done;
      }

      fam_u = _stack_peek(&sac_u);

      if ( MESA_NONE == fam_u->hed ) {
        fam_u->hed = cur;
        break;
      }
      else {
        c3_d pos_cel = fam_u->pos_d;
        cur = mesa_cell(fam_u->hed, cur);
        _stack_pop(&sac_u);

        if ( _cue_map_is_end(_cue_map_insert(&map_u, pos_cel, mesa_ref(cur))) ) {
          u3_assert(!"cue: out of memory");
        }
      }
    }
  }

fail:
  while ( sac_u.len_z ) {
    _cue_frame* fam_u = _stack_peek(&sac_u);
    mesa_unref(fam_u->hed);
    _stack_pop(&sac_u);
  }

done:
  for ( _cue_map_itr itr = _cue_map_first(&map_u);
        !_cue_map_is_end(itr);
        itr = _cue_map_next(itr) )
  {
    mesa_unref(itr.data->val);
  }
  _cue_map_cleanup(&map_u);
  _stack_free(&sac_u);

  return pro;
}

mesa_noun
mesa_cue(mesa_noun som)
{
  c3_y        tmp_y[8];
  c3_z        len_z;
  const c3_y* buf_y = _atom_view(som, tmp_y, &len_z);

  return mesa_cue_bytes(len_z, buf_y);
}

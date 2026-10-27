/// @file

#include "compat.h"
#include "mesa/noun.h"

static c3_i ret_i = 0;

#define N(x)   mesa_atom_u64(x)
#define T(s)   mesa_atom_cord(s)
#define C(a,b) mesa_cell(a, b)

//  jam and mug of each noun in _make(), computed by Hoon (urbit eval)
//
static const struct { const c3_c* jam_c; const c3_c* mug_c; } vec_u[] = {
  { "0x2", "0x79ff04e8" },
  { "0xc", "0x715c2a60" },
  { "0x48", "0x718b9468" },
  { "0x9b0", "0x31368e47" },
  { "0x7fffffffffffffff8100", "0x695c8310" },
  { "0xfffffffffffffffff80", "0x7a9f252e" },
  { "0x40000000000000000100", "0x135eb193" },
  { "0x800000000000000000002300", "0x58fb9948" },
  { "0x29", "0x192f5588" },
  { "0x1231", "0x6a36c1d9" },
  { "0x49c8c5", "0x396862ef" },
  { "0x24e000000000000000000008c01", "0xccabc49" },
  { "0x9a8e4de4c2c4f01dedeccf01", "0x66a42dbd" },
  { "0x32363937bb9037b63632b42f00dceadeddf01dbc19b237b7b47c072b3f8173f070b2b6387c072b3b0b9b9b2b6de01", "0x77e07595" },
  { "0xb8987433364dd12315", "0x4f6236cd" },
  { "0xcedcded840e6e8d2c440e4eadecc40f2e8f0d2e640dcc2d0e840cae4deda40f2d8cae8d2dcd2cccac840e6d240e8c2d0e840c8e4dec640cedcded840f2e4caec40c20f800", "0x55d33019" },
  { "0x1f41c126c9bf4281", "0x577e1ba7" }
};

static mesa_noun
_hex(const c3_c* hex_c)
{
  c3_z  len_z = strlen(hex_c + 2);
  c3_z  byt_z = (len_z + 1) / 2;
  c3_y* buf_y = c3_calloc(byt_z + 1);

  for ( c3_z i_z = 0; i_z < len_z; i_z++ ) {
    c3_c c = hex_c[2 + len_z - 1 - i_z];
    c3_y v = ( c <= '9' ) ? c - '0' : c - 'a' + 10;
    buf_y[i_z / 2] |= v << (4 * (i_z & 1));
  }

  mesa_noun pro = mesa_atom_bytes(byt_z, buf_y);
  c3_free(buf_y);
  return pro;
}

static mesa_noun
_big(c3_y hig_y, c3_z len_z)
{
  c3_y buf_y[32] = {0};
  buf_y[len_z - 1] = hig_y;
  return mesa_atom_bytes(len_z, buf_y);
}

static mesa_noun
_make(c3_h i_h)
{
  switch ( i_h ) {
    case 0:  return N(0);
    case 1:  return N(1);
    case 2:  return N(2);
    case 3:  return N(19);
    case 4:  return N(0xffffffffffffffffULL);
    case 5:  return N(0x7fffffffffffffffULL);
    case 6:  return N(0x8000000000000000ULL);
    case 7:  return _big(1, 11);
    case 8:  return C(N(0), N(0));
    case 9:  return C(N(1), N(2));
    case 10: return C(C(N(1), N(2)), C(N(1), N(2)));
    case 11: return C(_big(1, 11), _big(1, 11));
    case 12: return mesa_qual(T("foo"), T("bar"), T("foo"), T("bar"));
    case 13: return mesa_qual(T("message"), T("plea"), T("g"),
                     C(mesa_trel(T("ge"), T("hood"), N(0)),
                       mesa_qual(N(0), T("m"), T("noun"), T("hello world"))));
    case 14: return C(C(C(N(1), N(2)), N(3)),
                     C(C(C(N(1), N(2)), N(3)),
                       C(C(N(1), N(2)),
                         mesa_trel(N(3), N(4), N(5)))));
    case 15: return T("a very long cord that is definitely more than sixty four bits long");
    case 16: return mesa_qual(N(1000), N(1000), N(1000), N(2000));
    default: u3_assert(!"no such vector");
  }
}

static void
_test_vectors(void)
{
  for ( c3_h i_h = 0; i_h < sizeof(vec_u) / sizeof(vec_u[0]); i_h++ ) {
    mesa_noun som = _make(i_h);
    mesa_noun jam = mesa_jam(som);
    mesa_noun wan = _hex(vec_u[i_h].jam_c);
    mesa_noun mug = _hex(vec_u[i_h].mug_c);
    c3_d      mug_d;

    if ( !mesa_eq(jam, wan) ) {
      fprintf(stderr, "noun: vector %u: jam mismatch\r\n", i_h);
      ret_i = 1;
    }

    mesa_atom_get_u64(mug, &mug_d);
    if ( mesa_mug(som) != mug_d ) {
      fprintf(stderr, "noun: vector %u: mug have %x want %" PRIx64 "\r\n",
                      i_h, mesa_mug(som), mug_d);
      ret_i = 1;
    }

    {
      mesa_noun cue = mesa_cue(wan);
      if ( (MESA_NONE == cue) || !mesa_eq(cue, som) ) {
        fprintf(stderr, "noun: vector %u: cue mismatch\r\n", i_h);
        ret_i = 1;
      }
      mesa_unref(cue);
    }

    mesa_unref(som); mesa_unref(jam); mesa_unref(wan); mesa_unref(mug);
  }
}

static mesa_noun
_rand_noun(c3_h dep_h)
{
  c3_h r_h = rand();

  if ( (0 == dep_h) || (r_h % 3 == 0) ) {
    switch ( r_h % 4 ) {
      case 0:  return N(r_h % 8);
      case 1:  return N(((c3_d)rand() << 32) | rand());
      case 2:  return N(0x8000000000000000ULL | ((c3_d)rand() << 20));
      default: {
        c3_y buf_y[40];
        c3_z len_z = 1 + (rand() % sizeof(buf_y));
        for ( c3_z i_z = 0; i_z < len_z; i_z++ ) buf_y[i_z] = rand();
        return mesa_atom_bytes(len_z, buf_y);
      }
    }
  }

  {
    mesa_noun hed = _rand_noun(dep_h - 1);
    //  share subtrees sometimes, to exercise backreferences
    mesa_noun tel = ( r_h % 5 == 0 ) ? mesa_ref(hed) : _rand_noun(dep_h - 1);
    return C(hed, tel);
  }
}

static void
_test_roundtrip(void)
{
  for ( c3_h i_h = 0; i_h < 20000; i_h++ ) {
    mesa_noun som = _rand_noun(8);
    c3_z      len_z;
    c3_y*     buf_y = mesa_jam_bytes(som, &len_z);
    mesa_noun cue   = mesa_cue_bytes(len_z, buf_y);

    if ( (MESA_NONE == cue) || !mesa_eq(som, cue) || (mesa_mug(som) != mesa_mug(cue)) ) {
      fprintf(stderr, "noun: roundtrip %u failed\r\n", i_h);
      ret_i = 1;
      return;
    }

    c3_free(buf_y);
    mesa_unref(som);
    mesa_unref(cue);
  }
}

/* _test_big(): [small big] jams bit for bit as a plain bit writer
**              would (big atoms at every bit offset), and cues back
*/
typedef struct { c3_y* buf_y; c3_d pos_d; } _ref_w;

static void
_ref_bit(_ref_w* ref_u, c3_y bit_y)
{
  if ( bit_y ) ref_u->buf_y[ref_u->pos_d >> 3] |= (c3_y)(1 << (ref_u->pos_d & 7));
  ref_u->pos_d++;
}

static void
_ref_mat(_ref_w* ref_u, c3_z len_z, const c3_y* buf_y)
{
  c3_d b_d = 0, c_d = 0;

  for ( c3_d i_d = 0; i_d < (c3_d)len_z * 8; i_d++ ) {
    if ( (buf_y[i_d >> 3] >> (i_d & 7)) & 1 ) b_d = i_d + 1;
  }
  if ( !b_d ) { _ref_bit(ref_u, 1); return; }
  for ( c3_d t_d = b_d; t_d; t_d >>= 1 ) c_d++;
  for ( c3_d i_d = 0; i_d < c_d; i_d++ ) _ref_bit(ref_u, 0);
  _ref_bit(ref_u, 1);
  for ( c3_d i_d = 0; i_d + 1 < c_d; i_d++ ) _ref_bit(ref_u, (b_d >> i_d) & 1);
  for ( c3_d i_d = 0; i_d < b_d; i_d++ ) {
    _ref_bit(ref_u, (buf_y[i_d >> 3] >> (i_d & 7)) & 1);
  }
}

static void
_test_big(void)
{
  for ( c3_w i_w = 0; i_w < 2000; i_w++ ) {
    c3_y      sma_y[2] = { (c3_y)rand(), (c3_y)(rand() & 0x3f) };
    c3_z      len_z    = 9 + (rand() % 600);
    c3_y*     big_y    = c3_malloc(len_z);
    _ref_w    ref_u;
    c3_z      jaz_z;
    c3_y*     jam_y;
    mesa_noun som, cue;

    for ( c3_z j_z = 0; j_z < len_z; j_z++ ) big_y[j_z] = (c3_y)rand();
    big_y[len_z - 1] |= 1;
    som = mesa_cell(mesa_atom_bytes(2, sma_y), mesa_atom_bytes(len_z, big_y));

    ref_u.buf_y = c3_calloc(len_z + 64);
    ref_u.pos_d = 0;
    _ref_bit(&ref_u, 1);
    _ref_bit(&ref_u, 0);
    _ref_bit(&ref_u, 0);
    _ref_mat(&ref_u, 2, sma_y);
    _ref_bit(&ref_u, 0);
    _ref_mat(&ref_u, len_z, big_y);

    jam_y = mesa_jam_bytes(som, &jaz_z);
    if ( (jaz_z != ((ref_u.pos_d + 7) >> 3)) || memcmp(jam_y, ref_u.buf_y, jaz_z) ) {
      fprintf(stderr, "noun: big jam %u differs\r\n", i_w);
      ret_i = 1;
    }
    cue = mesa_cue_bytes(jaz_z, jam_y);
    if ( (MESA_NONE == cue) || !mesa_eq(som, cue) ) {
      fprintf(stderr, "noun: big roundtrip %u failed\r\n", i_w);
      ret_i = 1;
    }
    if ( MESA_NONE != cue ) mesa_unref(cue);
    mesa_unref(som);
    c3_free(jam_y);
    c3_free(ref_u.buf_y);
    c3_free(big_y);
    if ( ret_i ) return;
  }
}

static void
_test_deep(void)
{
  //  a million-element list, and a million-deep left-nested tree
  //
  mesa_noun lis = 0, lef = 0;

  for ( c3_h i_h = 0; i_h < 1000000; i_h++ ) {
    lis = C(N(i_h), lis);
    lef = C(lef, N(i_h & 7));
  }

  mesa_noun jil = mesa_jam(lis), jel = mesa_jam(lef);
  mesa_noun cil = mesa_cue(jil), cel = mesa_cue(jel);

  if ( (MESA_NONE == cil) || !mesa_eq(lis, cil) ) {
    fprintf(stderr, "noun: deep list roundtrip failed\r\n");
    ret_i = 1;
  }
  if ( (MESA_NONE == cel) || !mesa_eq(lef, cel) || (mesa_mug(lef) != mesa_mug(cel)) ) {
    fprintf(stderr, "noun: deep tree roundtrip failed\r\n");
    ret_i = 1;
  }

  mesa_unref(lis); mesa_unref(lef);
  mesa_unref(jil); mesa_unref(jel);
  mesa_unref(cil); mesa_unref(cel);
}

static void
_test_garbage(void)
{
  //  cue must reject or accept arbitrary bytes without crashing
  //
  for ( c3_h i_h = 0; i_h < 200000; i_h++ ) {
    c3_y buf_y[24];
    c3_z len_z = 1 + (rand() % sizeof(buf_y));
    for ( c3_z i_z = 0; i_z < len_z; i_z++ ) buf_y[i_z] = rand();
    mesa_unref(mesa_cue_bytes(len_z, buf_y));
  }

  //  truncated and dangling-backreference inputs
  //
  {
    c3_y bad_y[] = { 0x01 };          //  cell tag, no children
    c3_y bak_y[] = { 0x0f, 0x00 };    //  backref to position 1 that doesn't exist
    if ( MESA_NONE != mesa_cue_bytes(sizeof(bad_y), bad_y) ) {
      fprintf(stderr, "noun: accepted truncated cell\r\n");
      ret_i = 1;
    }
    if ( MESA_NONE != mesa_cue_bytes(sizeof(bak_y), bak_y) ) {
      fprintf(stderr, "noun: accepted dangling backreference\r\n");
      ret_i = 1;
    }
  }
}

static void
_test_at(void)
{
  mesa_noun som = mesa_trel(N(1), N(2), N(3));
  c3_d      v_d;

  u3_assert( mesa_atom_get_u64(mesa_at(2, som), &v_d) && (1 == v_d) );
  u3_assert( mesa_atom_get_u64(mesa_at(6, som), &v_d) && (2 == v_d) );
  u3_assert( mesa_atom_get_u64(mesa_at(7, som), &v_d) && (3 == v_d) );
  u3_assert( MESA_NONE == mesa_at(4, som) );
  u3_assert( MESA_NONE == mesa_at(0, som) );
  mesa_unref(som);
}

int
main()
{
  srand(getpid());

  fprintf(stderr, "noun: vectors\r\n");
  _test_vectors();
  fprintf(stderr, "noun: at\r\n");
  _test_at();
  fprintf(stderr, "noun: roundtrip\r\n");
  _test_roundtrip();
  fprintf(stderr, "noun: big atoms\r\n");
  _test_big();
  fprintf(stderr, "noun: deep\r\n");
  _test_deep();
  fprintf(stderr, "noun: garbage\r\n");
  _test_garbage();

  if ( !ret_i ) {
    fprintf(stderr, "noun: ok\r\n");
  }
  return ret_i;
}

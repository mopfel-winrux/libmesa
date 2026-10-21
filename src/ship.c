/// @file

#include "compat.h"
#include "murmur3.h"
#include "ship.h"

//  +po syllables (hoon.hoon)
//
static const c3_c* sis_c =
  "dozmarbinwansamlitsighidfidlissogdirwacsabwissib"
  "rigsoldopmodfoglidhopdardorlorhodfolrintogsilmir"
  "holpaslacrovlivdalsatlibtabhanticpidtorbolfosdot"
  "losdilforpilramtirwintadbicdifrocwidbisdasmidlop"
  "rilnardapmolsanlocnovsitnidtipsicropwitnatpanmin"
  "ritpodmottamtolsavposnapnopsomfinfonbanmorworsip"
  "ronnorbotwicsocwatdolmagpicdavbidbaltimtasmallig"
  "sivtagpadsaldivdactansidfabtarmonranniswolmispal"
  "lasdismaprabtobrollatlonnodnavfignomnibpagsopral"
  "bilhaddocridmocpacravripfaltodtiltinhapmicfanpat"
  "taclabmogsimsonpinlomrictapfirhasbosbatpochactid"
  "havsaplindibhosdabbitbarracparloddosbortochilmac"
  "tomdigfilfasmithobharmighinradmashalraglagfadtop"
  "mophabnilnosmilfopfamdatnoldinhatnacrisfotribhoc"
  "nimlarfitwalrapsarnalmoslandondanladdovrivbacpol"
  "laptalpitnambonrostonfodponsovnocsorlavmatmipfip";

static const c3_c* dex_c =
  "zodnecbudwessevpersutletfulpensytdurwepserwylsun"
  "rypsyxdyrnuphebpeglupdepdysputlughecryttyvsydnex"
  "lunmeplutseppesdelsulpedtemledtulmetwenbynhexfeb"
  "pyldulhetmevruttylwydtepbesdexsefwycburderneppur"
  "rysrebdennutsubpetrulsynregtydsupsemwynrecmegnet"
  "secmulnymtevwebsummutnyxrextebfushepbenmuswyxsym"
  "selrucdecwexsyrwetdylmynmesdetbetbeltuxtugmyrpel"
  "syptermebsetdutdegtexsurfeltudnuxruxrenwytnubmed"
  "lytdusnebrumtynseglyxpunresredfunrevrefmectedrus"
  "bexlebduxrynnumpyxrygryxfeptyrtustyclegnemfermer"
  "tenlusnussyltecmexpubrymtucfyllepdebbermughuttun"
  "bylsudpemdevlurdefbusbeprunmelpexdytbyttyplevmyl"
  "wedducfurfexnulluclennerlexrupnedlecrydlydfenwel"
  "nydhusrelrudneshesfetdesretdunlernyrsebhulryllud"
  "remlysfynwerrycsugnysnyllyndyndemluxfedsedbecmun"
  "lyrtesmudnytbyrsenwegfyrmurtelreptegpecnelnevfes";

//  +ob constants, from vere/pkg/noun/jets/e/{fein,fynd}_ob.c
//
static const c3_h a_h =     0xffff;
static const c3_h b_h =    0x10000;
static const c3_h k_h = 0xffff0000;
static const c3_h rak_h[4] = { 0xb76d5eed, 0xee281300, 0x85bcae01, 0x4b387af7 };
static const c3_h kar_h[4] = { 0x4b387af7, 0x85bcae01, 0xee281300, 0xb76d5eed };

/* _fe_ob(): +fe:ob, correct over [0x0, 0xfffe.ffff]
*/
static c3_h
_fe_ob(c3_h m_h)
{
  c3_h l_h = m_h % a_h;
  c3_h r_h = m_h / a_h;
  c3_h f_h, t_h;
  c3_y j_y, k_y[2];

  for ( j_y = 0; j_y < 4; j_y++ ) {
    k_y[0] = r_h & 0xff;
    k_y[1] = (r_h >> 8) & 0xff;

    MurmurHash3_x86_32(k_y, 2, rak_h[j_y], &f_h);

    //  NB: this addition can overflow a c3_h (before mod)
    //
    t_h = ((c3_d)f_h + l_h) % (!(j_y & 1) ? a_h : b_h);
    l_h = r_h;
    r_h = t_h;
  }

  //  legendary @max19
  //
  return ( a_h == r_h )
         ? (r_h * a_h) + l_h
         : (l_h * a_h) + r_h;
}

/* _fen_ob(): +fen:ob, correct over [0x0, 0xfffe.ffff]
*/
static c3_h
_fen_ob(c3_h m_h)
{
  c3_h l_h = m_h / a_h;
  c3_h r_h = m_h % a_h;
  c3_h f_h, t_h;
  c3_y j_y, k_y[2];

  //  legendary @max19
  //
  if ( a_h == l_h ) {
    t_h = l_h;
    l_h = r_h;
    r_h = t_h;
  }

  for ( j_y = 0; j_y < 4; j_y++ ) {
    k_y[0] = l_h & 0xff;
    k_y[1] = (l_h >> 8) & 0xff;

    MurmurHash3_x86_32(k_y, 2, kar_h[j_y], &f_h);

    t_h = ( j_y & 1 )
          ? ((r_h + a_h) - (f_h % a_h)) % a_h
          : ((r_h + b_h) - (f_h % b_h)) % b_h;
    r_h = l_h;
    l_h = t_h;
  }

  return (r_h * a_h) + l_h;
}

static c3_h
_feis_ob(c3_h m_h)
{
  c3_h c_h = _fe_ob(m_h - b_h);
  return b_h + (( c_h < k_h ) ? c_h : _fe_ob(c_h));
}

static c3_h
_tail_ob(c3_h m_h)
{
  c3_h c_h = _fen_ob(m_h - b_h);
  return b_h + (( c_h < k_h ) ? c_h : _fen_ob(c_h));
}

/* u3_ob_fein(): +fein:ob, for atoms of at most 64 bits.
*/
c3_d
u3_ob_fein(c3_d pyn_d)
{
  c3_h lo_h = pyn_d & 0xffffffff;

  if ( lo_h < b_h ) {
    return pyn_d;
  }
  return (pyn_d & 0xffffffff00000000ULL) | _feis_ob(lo_h);
}

/* u3_ob_fynd(): +fynd:ob, for atoms of at most 64 bits.
*/
c3_d
u3_ob_fynd(c3_d pyn_d)
{
  c3_h lo_h = pyn_d & 0xffffffff;

  if ( lo_h < b_h ) {
    return pyn_d;
  }
  return (pyn_d & 0xffffffff00000000ULL) | _tail_ob(lo_h);
}

static inline void
_s_chub_to_bytes(c3_y byt_y[8], c3_d num_d)
{
  byt_y[0] = num_d & 0xff;
  byt_y[1] = (num_d >>  8) & 0xff;
  byt_y[2] = (num_d >> 16) & 0xff;
  byt_y[3] = (num_d >> 24) & 0xff;
  byt_y[4] = (num_d >> 32) & 0xff;
  byt_y[5] = (num_d >> 40) & 0xff;
  byt_y[6] = (num_d >> 48) & 0xff;
  byt_y[7] = (num_d >> 56) & 0xff;
}

static inline c3_d
_s_bytes_to_chub(c3_y byt_y[8])
{
  return (c3_d)byt_y[0]
       | (c3_d)byt_y[1] << 8
       | (c3_d)byt_y[2] << 16
       | (c3_d)byt_y[3] << 24
       | (c3_d)byt_y[4] << 32
       | (c3_d)byt_y[5] << 40
       | (c3_d)byt_y[6] << 48
       | (c3_d)byt_y[7] << 56;
}

void
u3_ship_to_bytes(u3_ship who_u, c3_y len_y, c3_y* buf_y)
{
  c3_y sip_y[16] = {0};

  _s_chub_to_bytes(sip_y, who_u[0]);
  _s_chub_to_bytes(sip_y + 8, who_u[1]);

  memcpy(buf_y, sip_y, c3_min(16, len_y));
}

void
u3_ship_of_bytes(u3_ship who_u, c3_y len_y, c3_y* buf_y)
{
  c3_y sip_y[16] = {0};
  memcpy(sip_y, buf_y, c3_min(16, len_y));

  who_u[0] = _s_bytes_to_chub(sip_y);
  who_u[1] = _s_bytes_to_chub(sip_y + 8);
}

/* _ship_fein(): +fein:ob over a full 128-bit ship.
*/
static void
_ship_fein(u3_ship out_u, u3_ship who_u)
{
  out_u[0] = who_u[0];
  out_u[1] = who_u[1];

  //  +fein:ob leaves atoms wider than 64 bits (comets) unscrambled
  //
  if ( 0 == who_u[1] ) {
    out_u[0] = u3_ob_fein(who_u[0]);
  }
}

static c3_s
_ship_chunk(u3_ship who_u, c3_y i_y)
{
  return (who_u[i_y >> 2] >> (16 * (i_y & 3))) & 0xffff;
}

c3_c*
u3_ship_to_string(u3_ship who_u)
{
  u3_ship sxz_u;
  c3_y    dyy_y;
  c3_c*   str_c = c3_malloc(64);
  c3_c*   cur_c = str_c;

  _ship_fein(sxz_u, who_u);

  *cur_c++ = '~';

  //  galaxy: single suffix syllable
  //
  if ( (0 == sxz_u[1]) && (sxz_u[0] < 0x100) ) {
    memcpy(cur_c, dex_c + 3 * sxz_u[0], 3);
    cur_c[3] = 0;
    return str_c;
  }

  //  number of 16-bit chunks, +met 4
  //
  dyy_y = sxz_u[1]
        ? 4 + ((c3_bits_chub(sxz_u[1]) + 15) >> 4)
        : ((c3_bits_chub(sxz_u[0]) + 15) >> 4);

  for ( c3_y imp_y = dyy_y; imp_y-- > 0; ) {
    c3_s log_s = _ship_chunk(sxz_u, imp_y);

    memcpy(cur_c,     sis_c + 3 * (log_s >> 8),   3);
    memcpy(cur_c + 3, dex_c + 3 * (log_s & 0xff), 3);
    cur_c += 6;

    if ( imp_y ) {
      if ( 0 == (imp_y % 4) ) {
        *cur_c++ = '-';
      }
      *cur_c++ = '-';
    }
  }

  *cur_c = 0;
  return str_c;
}

static c3_i
_syl_find(const c3_c* tab_c, const c3_c* syl_c)
{
  for ( c3_i i_i = 0; i_i < 256; i_i++ ) {
    if ( 0 == memcmp(tab_c + 3 * i_i, syl_c, 3) ) {
      return i_i;
    }
  }
  return -1;
}

c3_o
u3_ship_of_string(u3_ship who_u, const c3_c* str_c)
{
  u3_ship sxz_u = {0, 0};
  c3_c    can_c[64];
  c3_z    len_z;
  c3_y    nun_y = 0;

  if ( '~' == *str_c ) {
    str_c++;
  }

  len_z = strlen(str_c);

  if ( 3 == len_z ) {
    c3_i dex_i = _syl_find(dex_c, str_c);
    if ( 0 > dex_i ) {
      return c3n;
    }
    who_u[0] = dex_i;
    who_u[1] = 0;
    return c3y;
  }

  //  collect 16-bit chunks, most significant first
  //
  {
    const c3_c* cur_c = str_c;

    while ( *cur_c ) {
      c3_i pre_i, suf_i;

      if ( 8 <= nun_y ) {
        return c3n;
      }
      if ( (strlen(cur_c) < 6) ) {
        return c3n;
      }
      if (  (0 > (pre_i = _syl_find(sis_c, cur_c)))
         || (0 > (suf_i = _syl_find(dex_c, cur_c + 3))) )
      {
        return c3n;
      }

      //  shift left by 16 bits and append
      //
      sxz_u[1] = (sxz_u[1] << 16) | (sxz_u[0] >> 48);
      sxz_u[0] = (sxz_u[0] << 16) | (c3_d)((pre_i << 8) | suf_i);
      nun_y++;

      cur_c += 6;
      while ( '-' == *cur_c ) {
        cur_c++;
      }
    }
  }

  if ( 0 == nun_y ) {
    return c3n;
  }

  if ( 0 == sxz_u[1] ) {
    who_u[0] = u3_ob_fynd(sxz_u[0]);
    who_u[1] = 0;
  }
  else {
    who_u[0] = sxz_u[0];
    who_u[1] = sxz_u[1];
  }

  //  only accept the canonical rendering
  //
  {
    c3_c* ren_c = u3_ship_to_string(who_u);
    c3_o  ok_o;

    snprintf(can_c, sizeof(can_c), "~%s", str_c);
    ok_o = __(0 == strcmp(ren_c, can_c));
    c3_free(ren_c);
    return ok_o;
  }
}

c3_o
u3_ships_equal(u3_ship sip_u, u3_ship sap_u)
{
  return __((sip_u[0] == sap_u[0]) && (sip_u[1] == sap_u[1]));
}

void
u3_ship_copy(u3_ship des_u, u3_ship src_u)
{
  des_u[0] = src_u[0];
  des_u[1] = src_u[1];
}

c3_h
u3_ship_rank(u3_ship who_u)
{
  if      ( who_u[1] )       return c3__pawn;
  else if ( who_u[0] >> 32 ) return c3__earl;
  else if ( who_u[0] >> 16 ) return c3__duke;
  else if ( who_u[0] >> 8 )  return c3__king;
  else                       return c3__czar;
}

void
u3_ship_sein(u3_ship par_u, u3_ship who_u)
{
  par_u[1] = 0;

  switch ( u3_ship_rank(who_u) ) {
    case c3__czar: par_u[0] = who_u[0];              break;
    case c3__king: par_u[0] = who_u[0] & 0xff;       break;
    case c3__duke: par_u[0] = who_u[0] & 0xffff;     break;
    case c3__earl: par_u[0] = who_u[0] & 0xffffffff; break;
    case c3__pawn: par_u[0] = who_u[0] & 0xffff;     break;
  }
}

c3_y
u3_ship_czar(u3_ship who_u) { return who_u[0] & 0xFF; }

c3_s
u3_ship_king(u3_ship who_u) { return who_u[0] & 0xffff; }

c3_h
u3_ship_duke(u3_ship who_u) { return who_u[0] & 0xffffffff; }

c3_d
u3_ship_earl(u3_ship who_u) { return who_u[0]; }

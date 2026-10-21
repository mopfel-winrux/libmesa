/// @file

#include "compat.h"
#include "ship.h"

static c3_i ret_i = 0;

static void
_expect_render(c3_d hi_d, c3_d lo_d, const c3_c* wan_c)
{
  u3_ship who_u = { lo_d, hi_d };
  u3_ship hav_u;
  c3_c*   str_c = u3_ship_to_string(who_u);

  if ( 0 != strcmp(str_c, wan_c) ) {
    fprintf(stderr, "ship: render %" PRIx64 ":%" PRIx64 " have %s want %s\r\n",
                    hi_d, lo_d, str_c, wan_c);
    ret_i = 1;
  }

  if (  (c3n == u3_ship_of_string(hav_u, wan_c))
     || (c3n == u3_ships_equal(hav_u, who_u)) )
  {
    fprintf(stderr, "ship: parse %s failed\r\n", wan_c);
    ret_i = 1;
  }

  c3_free(str_c);
}

static void
_expect_reject(const c3_c* str_c)
{
  u3_ship who_u;

  if ( c3y == u3_ship_of_string(who_u, str_c) ) {
    fprintf(stderr, "ship: accepted bad @p %s\r\n", str_c);
    ret_i = 1;
  }
}

static void
_test_ob_roundtrip(void)
{
  for ( c3_d i_d = 0; i_d < 1000000; i_d++ ) {
    c3_d pyn_d = ((c3_d)rand() << 32) ^ (c3_d)rand() ^ i_d;

    if ( u3_ob_fynd(u3_ob_fein(pyn_d)) != pyn_d ) {
      fprintf(stderr, "ship: fein/fynd roundtrip failed for %" PRIx64 "\r\n",
                      pyn_d);
      ret_i = 1;
      return;
    }
  }
}

static void
_test_string_roundtrip(void)
{
  for ( c3_h i_h = 0; i_h < 100000; i_h++ ) {
    u3_ship who_u, hav_u;
    c3_c*   str_c;

    switch ( i_h % 5 ) {
      case 0: who_u[0] = rand() & 0xff;   who_u[1] = 0; break;
      case 1: who_u[0] = rand() & 0xffff; who_u[1] = 0; break;
      case 2: who_u[0] = (c3_h)rand();    who_u[1] = 0; break;
      case 3: who_u[0] = ((c3_d)rand() << 32) | (c3_h)rand(); who_u[1] = 0; break;
      case 4: who_u[0] = ((c3_d)rand() << 32) | (c3_h)rand();
              who_u[1] = ((c3_d)rand() << 32) | (c3_h)rand() | 1;
              break;
    }

    str_c = u3_ship_to_string(who_u);

    if (  (c3n == u3_ship_of_string(hav_u, str_c))
       || (c3n == u3_ships_equal(hav_u, who_u)) )
    {
      fprintf(stderr, "ship: string roundtrip failed for %s\r\n", str_c);
      ret_i = 1;
      c3_free(str_c);
      return;
    }

    c3_free(str_c);
  }
}

static void
_test_sein(void)
{
  u3_ship who_u, par_u, wan_u;

  u3_assert( c3y == u3_ship_of_string(who_u, "~sampel-palnet") );
  u3_ship_sein(par_u, who_u);
  u3_assert( c3y == u3_ship_of_string(wan_u, "~talpur") );
  if ( c3n == u3_ships_equal(par_u, wan_u) ) {
    c3_c* str_c = u3_ship_to_string(par_u);
    fprintf(stderr, "ship: sein ~sampel-palnet = %s, want ~talpur\r\n", str_c);
    c3_free(str_c);
    ret_i = 1;
  }
}

int
main()
{
  srand(getpid());

  _expect_render(0, 0,          "~zod");
  _expect_render(0, 1,          "~nec");
  _expect_render(0, 255,        "~fes");
  _expect_render(0, 256,        "~marzod");
  _expect_render(0, 768,        "~wanzod");
  _expect_render(0, 0xffff,     "~fipfes");
  _expect_render(0, 1624961343, "~sampel-palnet");
  _expect_render(0, 0x100000000ULL + 1624961343, "~doznec-sampel-palnet");

  _expect_reject("~");
  _expect_reject("~zoz");
  _expect_reject("~dozzod");          //  non-canonical: leading zero chunk
  _expect_reject("~sampel--palnet");
  _expect_reject("~sampelpalnet");
  _expect_reject("~sampel-palne");

  _test_sein();
  _test_ob_roundtrip();
  _test_string_roundtrip();

  if ( !ret_i ) {
    fprintf(stderr, "ship: ok\r\n");
  }
  return ret_i;
}

/// @file

#include "compat.h"
#include "json.h"
#include "mesa/azimuth.h"

static c3_i ret_i = 0;

static void
_expect(c3_t ok_t, const c3_c* msg_c)
{
  if ( !ok_t ) {
    fprintf(stderr, "azimuth: %s\r\n", msg_c);
    ret_i = 1;
  }
}

//  live responses from roller.urbit.org, 2026-10-21
//
static const c3_c* zod_c =
  "{\"id\":\"1\",\"jsonrpc\":\"2.0\",\"result\":{\"dominion\":\"l1\","
  "\"ownership\":{\"transferProxy\":{\"address\":\"0x0000000000000000000000000000000000000000\",\"nonce\":0},"
  "\"owner\":{\"address\":\"0x9f57c77b1095bd5db0558b9cb9b8e6fc67375e3c\",\"nonce\":0}},"
  "\"network\":{\"keys\":{\"life\":\"6\",\"suite\":\"1\","
  "\"auth\":\"0xbd44cee24b74632e65d842ce0fc0bf6e109675e6aca4a9d94c33657e1cc24231\","
  "\"crypt\":\"0xa5f1a3a6662b675c109be8ce642c1b4add53df18609738b8d59bbc1d8d508831\"},"
  "\"sponsor\":{\"has\":true,\"who\":0},\"rift\":\"2\"}}}";

static const c3_c* planet_c =
  "{\"id\":\"1\",\"jsonrpc\":\"2.0\",\"result\":{\"dominion\":\"l1\",\"ownership\":{},"
  "\"network\":{\"keys\":{\"life\":\"8\",\"suite\":\"1\","
  "\"auth\":\"0x2e636cb9d3ef1f06f49641ba0fddf74be8ef2f1bfe7a5db44d90396e72bfae90\","
  "\"crypt\":\"0xc3abfa6750f787bbeb55af21cd59251b8d97973ffe248837abb8568caa9a304e\"},"
  "\"sponsor\":{\"has\":true,\"who\":1024},\"rift\":\"7\"}}}";

static const c3_c* missing_c =
  "{\"id\":\"1\",\"error\":{\"code\":-32000,\"message\":\"Resource not found\"},"
  "\"jsonrpc\":\"2.0\"}";

static const c3_c* dns_c =
  "{\"id\":\"t\",\"jsonrpc\":\"2.0\",\"result\":[\"urbit.org\",\"urbit.org\",\"urbit.org\"]}";

static mesa_ship
_ship(const c3_c* str_c)
{
  mesa_ship who;
  u3_assert( mesa_ship_parse(str_c, &who) );
  return who;
}

static void
_test_point(void)
{
  mesa_point pot_u;

  _expect(MESA_AZIMUTH_OK == mesa_azimuth_point_parse(
            _ship("~zod"), strlen(zod_c), zod_c, &pot_u), "parse zod");
  _expect(2 == pot_u.rift && 6 == pot_u.life, "zod rift/life");
  _expect(pot_u.has_sponsor && 0 == pot_u.sponsor.lo, "zod sponsor");

  //  (cat 3 'b' (cat 8 auth crypt)): key bytes are the atoms' LE bytes
  //
  _expect('b' == pot_u.pass[0], "pass suite");
  _expect(0x31 == pot_u.pass[1] && 0xbd == pot_u.pass[32], "auth LE");
  _expect(0x31 == pot_u.pass[33] && 0xa5 == pot_u.pass[64], "crypt LE");

  _expect(MESA_AZIMUTH_OK == mesa_azimuth_point_parse(
            _ship("~ravmel-ropdyl"), strlen(planet_c), planet_c, &pot_u),
          "parse planet");
  _expect(7 == pot_u.rift && 8 == pot_u.life, "planet rift/life");
  _expect(mesa_ship_eq(pot_u.sponsor, _ship("~samzod")), "planet sponsor");

  _expect(MESA_AZIMUTH_NOT_FOUND == mesa_azimuth_point_parse(
            _ship("~sampel-palnet"), strlen(missing_c), missing_c, &pot_u),
          "unspawned");

  //  every truncation of a good response is rejected, never crashes
  //
  for ( c3_z i_z = 0; i_z < strlen(zod_c); i_z++ ) {
    _expect(MESA_AZIMUTH_BAD == mesa_azimuth_point_parse(
              _ship("~zod"), i_z, zod_c, &pot_u), "truncated");
  }

  //  unknown suite gives an empty key
  //
  {
    c3_c* sut_c = strdup(zod_c);
    c3_c* hit_c = strstr(sut_c, "\"suite\":\"1\"");
    hit_c[9] = '2';
    _expect(MESA_AZIMUTH_OK == mesa_azimuth_point_parse(
              _ship("~zod"), strlen(sut_c), sut_c, &pot_u), "suite 2");
    _expect(0 == pot_u.pass[1] && 0 == pot_u.pass[64], "suite 2 keyless");
    c3_free(sut_c);
  }

  {
    c3_c* req_c = mesa_azimuth_point_request(_ship("~sampel-palnet"));
    _expect(0 == strcmp(req_c,
      "{\"jsonrpc\":\"2.0\",\"id\":\"~sampel-palnet\",\"method\":\"getPoint\","
      "\"params\":{\"ship\":\"~sampel-palnet\"}}"), "request");
    c3_free(req_c);
  }
}

static void
_test_dns(void)
{
  c3_c* dom_c[4];
  c3_i  n_i = mesa_azimuth_dns_parse(strlen(dns_c), dns_c, dom_c, 4);

  _expect(3 == n_i, "dns count");
  for ( c3_i i = 0; i < n_i; i++ ) {
    _expect(0 == strcmp("urbit.org", dom_c[i]), "dns domain");
    c3_free(dom_c[i]);
  }
}

static void
_test_json(void)
{
  json* jon_u;

  const c3_c* txt_c = "{\"a\":[1,2,{\"b\":\"x\\u00e9\\n\"}],\"c\":null}";

  jon_u = json_parse(strlen(txt_c), txt_c);
  _expect(0 != jon_u, "parse");
  _expect(0 == strcmp("x\xc3\xa9\n",
            json_str(json_get(json_get(jon_u, "a")->val_u[2], "b"))), "unescape");
  json_free(jon_u);

  _expect(0 == json_parse(3, "[1,"), "unterminated");
  _expect(0 == json_parse(5, "[1]x "), "trailing");
  _expect(0 == json_parse(8, "\"\\ud800\""), "surrogate");

  //  deep nesting is refused, not a stack overflow
  //
  {
    c3_z  len_z = 100000;
    c3_c* txt_c = c3_malloc(len_z);
    memset(txt_c, '[', len_z);
    _expect(0 == json_parse(len_z, txt_c), "deep");
    c3_free(txt_c);
  }

  //  random junk
  //
  {
    c3_c buf_c[64];
    const c3_c alf_c[] = "{}[]\",:0123456789-.eEtruefalsn \\u";
    srand(7);
    for ( c3_w i = 0; i < 200000; i++ ) {
      c3_z len_z = rand() % sizeof(buf_c);
      for ( c3_z j = 0; j < len_z; j++ ) {
        buf_c[j] = alf_c[rand() % (sizeof(alf_c) - 1)];
      }
      json_free(json_parse(len_z, buf_c));
    }
  }
}

int
main(int argc, char* argv[])
{
  _test_point();
  _test_dns();
  _test_json();

  if ( ret_i ) {
    fprintf(stderr, "azimuth: failed\r\n");
  }
  else {
    fprintf(stderr, "azimuth ok\r\n");
  }
  return ret_i;
}

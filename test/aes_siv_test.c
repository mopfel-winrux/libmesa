/// @file
///
/// AES-256-SIV against Hoon's ++sivc:aes:crypto (vectors from `urbit eval`).

#include "aes_siv.h"

#include <stdio.h>
#include <string.h>

static int ret_i = 0;

#define expect(c, ...) do { if ( !(c) ) { fprintf(stderr, "aes_siv: %s:%d: ", \
  __FILE__, __LINE__); fprintf(stderr, __VA_ARGS__); fprintf(stderr, "\r\n"); \
  ret_i = 1; } } while (0)

/* _atom(): little-endian bytes of a big-endian hex atom; returns length */
static size_t
_atom(const char* hex, unsigned char* out)
{
  size_t n = strlen(hex) / 2;
  for ( size_t i = 0; i < n; i++ ) {
    unsigned v;
    sscanf(hex + 2 * i, "%2x", &v);
    out[n - 1 - i] = (unsigned char)v;
  }
  return n;
}

int
main(void)
{
  //  =+  k=(shaz 0x1234.5678)
  //  (~(en sivc:aes:crypto k ~[0x1.0203 0x5 0]) 0xdead.beef.0011...0102)
  //
  unsigned char key[64] = {0}, msg[22], iv[16], cyf[22], wiv[16], wcy[22], pln[22];
  unsigned char a0[3], a1[1];
  const c3_y*   ads[3] = { a0, a1, a1 };
  c3_z          adz[3];

  _atom("a07af86d4a0066f1bcc45e2db67872d05369edf6fc97f70ab056c27952dddb9a"
        "ba617770253b8523cace0317c960b02f5366f7fe60ebdf013e18bb8eaaaab471", key);
  adz[0] = _atom("010203", a0);
  adz[1] = _atom("05", a1);
  adz[2] = 0;
  _atom("deadbeef00112233445566778899aabbccddeeff0102", msg);
  _atom("d45cf07676712523d380fb7dd8dd57b3", wiv);
  _atom("8dd68af6e1f9f8d856025e84b978cdfc52050424c1e9", wcy);

  mesa_sivc_en(key, 3, ads, adz, msg, 22, iv, cyf);
  expect(!memcmp(iv, wiv, 16), "siv");
  expect(!memcmp(cyf, wcy, 22), "ciphertext");

  expect(c3y == mesa_sivc_de(key, 3, ads, adz, iv, cyf, 22, pln), "decrypts");
  expect(!memcmp(pln, msg, 22), "plaintext");

  cyf[3] ^= 1;
  expect(c3n == mesa_sivc_de(key, 3, ads, adz, iv, cyf, 22, pln), "tampered ciphertext");
  cyf[3] ^= 1;
  adz[2] = 1;
  expect(c3n == mesa_sivc_de(key, 3, ads, adz, iv, cyf, 22, pln), "other data");

  fprintf(stderr, ret_i ? "aes_siv: failed\r\n" : "aes_siv ok\r\n");
  return ret_i;
}

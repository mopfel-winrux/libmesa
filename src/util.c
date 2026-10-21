/// @file

#include "compat.h"
#include "murmur3.h"
#include "mesa/log.h"

static mesa_log_f  log_f;
static void*       log_v;

void
mesa_set_log(mesa_log_f fun_f, void* ctx_v)
{
  log_f = fun_f;
  log_v = ctx_v;
}

void
mesa_log(const c3_c* fmt_c, ...)
{
  c3_c    buf_c[1024];
  va_list ap;

  va_start(ap, fmt_c);
  vsnprintf(buf_c, sizeof(buf_c), fmt_c, ap);
  va_end(ap);

  if ( log_f ) {
    log_f(log_v, buf_c);
  }
  else {
    fprintf(stderr, "%s\r\n", buf_c);
  }
}

_Noreturn void
mesa_fail(const c3_c* fil_c, c3_i lin_i, const c3_c* exp_c)
{
  mesa_log("mesa: assertion failed: %s at %s:%d", exp_c, fil_c, lin_i);
  abort();
}

/* mesa_mug_bytes(): +mug of a byte buffer, LSB first.
**
**   identical to u3r_mug_bytes() in vere/pkg/noun/retrieve.c
*/
c3_m
mesa_mug_bytes(const c3_y* buf_y, c3_h len_h)
{
  c3_h syd_h = 0xcafebabe;
  c3_h   i_h = 0;

  while ( i_h < 8 ) {
    c3_h haz_h;
    c3_m ham_m;

    MurmurHash3_x86_32(buf_y, len_h, syd_h, &haz_h);
    ham_m = (haz_h >> 31) ^ (haz_h & 0x7fffffff);

    if ( 0 == ham_m ) {
      syd_h++; i_h++;
    }
    else {
      return ham_m;
    }
  }

  return 0x7fff;
}

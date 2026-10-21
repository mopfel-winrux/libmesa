/// @file
///
/// Shims for the handful of vere/u3 runtime facilities that code extracted
/// from vere still references (logging, assertions, mug). Keeping the
/// original call sites intact keeps the extracted code diffable against vere.

#ifndef MESA_COMPAT_H
#define MESA_COMPAT_H

#include "c3/c3.h"

/* mesa_log(): log a line through the host-installed log hook (default stderr).
*/
void
mesa_log(const c3_c* fmt_c, ...) __attribute__((format(printf, 1, 2)));

/* mesa_fail(): report a failed invariant and abort.
*/
_Noreturn void
mesa_fail(const c3_c* fil_c, c3_i lin_i, const c3_c* exp_c);

/* mesa_mug_bytes(): +mug of a byte buffer, LSB first (u3r_mug_bytes clone).
*/
c3_m
mesa_mug_bytes(const c3_y* buf_y, c3_h len_h);

#define u3l_log            mesa_log
#define u3_assert(x)       do { if ( !(x) ) mesa_fail(__FILE__, __LINE__, #x); } while (0)
#define u3m_bail(mot)      mesa_fail(__FILE__, __LINE__, "bail")
#define u3r_mug_bytes      mesa_mug_bytes

#endif /* ifndef MESA_COMPAT_H */

/// @file
///
/// Knots and paths: the text forms Hoon uses for path segments
/// (+scot/+slaw for @p, @ud and @uv) and path rendering.
///
/// A rendered path follows +spat: "/a/x/1//flow/0" ('' segments are
/// empty). Packet names carry the same text without the leading '/'.

#ifndef MESA_KNOT_H
#define MESA_KNOT_H

#include "c3/c3.h"
#include "ship.h"

#define KNOT_MAX  256     //  longest knot we render
#define PATH_MAX_SEGS 64

/* knot_ud(): (scot %ud n), e.g. "1.000"; returns length
*/
c3_z
knot_ud(c3_d num_d, c3_c out_c[32]);

/* knot_ud_parse(): (slaw %ud txt), canonical form only
*/
c3_o
knot_ud_parse(c3_z len_z, const c3_c* txt_c, c3_d* out_d);

/* knot_uv(): (scot %uv a) for little-endian bytes; malloc'd
*/
c3_c*
knot_uv(c3_z len_z, const c3_y* buf_y);

/* knot_uv_parse(): (slaw %uv txt) to little-endian bytes (malloc'd);
**                  NULL if not a canonical @uv.
*/
c3_y*
knot_uv_parse(c3_z len_z, const c3_c* txt_c, c3_z* out_z);

/* knot_p_parse(): (slaw %p txt), length-delimited
*/
c3_o
knot_p_parse(c3_z len_z, const c3_c* txt_c, u3_ship who_u);

/* path segments: a borrowed view into rendered path text
*/
typedef struct _knot_seg {
  const c3_c* str_c;
  c3_z        len_z;
} knot_seg;

typedef struct _knot_path {
  c3_w     len_w;
  knot_seg seg_u[PATH_MAX_SEGS];
} knot_path;

/* knot_path_split(): split "/a/b" or "a/b" into segments; c3n if there
**                    are too many or a segment isn't a valid knot.
*/
c3_o
knot_path_split(c3_z len_z, const c3_c* pat_c, knot_path* out_u);

/* knot_seg_is(): segment equals a C string
*/
c3_o
knot_seg_is(const knot_seg* seg_u, const c3_c* str_c);

/* knot_beam(): +en-beam for [[who %$ ud+1] pat], "/~who//1" ++ pat
**              ([pat_c] begins with '/'); malloc'd
*/
c3_c*
knot_beam(u3_ship who_u, const c3_c* pat_c);

#endif /* ifndef MESA_KNOT_H */

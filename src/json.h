/// @file
///
/// A small JSON reader, enough for azimuth RPC responses. Parses into a
/// tree of nodes; strings are unescaped, numbers are kept as text.

#ifndef MESA_JSON_H
#define MESA_JSON_H

#include "c3/c3.h"

typedef enum {
  JSON_NULL,
  JSON_BOOL,
  JSON_NUM,
  JSON_STR,
  JSON_ARR,
  JSON_OBJ,
} json_type;

typedef struct _json json;

struct _json {
  json_type typ_e;
  c3_o      bol_o;      //  JSON_BOOL
  c3_c*     str_c;      //  JSON_STR, JSON_NUM (text); NUL-terminated
  c3_z      len_z;      //  JSON_STR byte length; JSON_ARR/OBJ child count
  c3_c**    key_c;      //  JSON_OBJ keys
  json**    val_u;      //  JSON_ARR/OBJ children
};

/* json_parse(): parse [len_z] bytes; 0 if malformed or nested too deep.
*/
json*
json_parse(c3_z len_z, const c3_c* txt_c);

void
json_free(json* jon_u);

/* json_get(): object member [key_c], or 0.
*/
json*
json_get(const json* jon_u, const c3_c* key_c);

/* json_str(): string value, or 0 if [jon_u] isn't a string.
*/
const c3_c*
json_str(const json* jon_u);

/* json_u64(): a decimal number, or a string of one; c3n if neither.
*/
c3_o
json_u64(const json* jon_u, c3_d* out_d);

#endif /* ifndef MESA_JSON_H */

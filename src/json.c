/// @file

#include "compat.h"
#include "json.h"

#define JSON_DEPTH 64

typedef struct {
  const c3_c* txt_c;
  c3_z        len_z;
  c3_z        pos_z;
} _json_in;

static json* _json_value(_json_in* in_u, c3_w dep_w);

static void
_json_skip(_json_in* in_u)
{
  while ( in_u->pos_z < in_u->len_z ) {
    c3_c c = in_u->txt_c[in_u->pos_z];
    if ( (' ' != c) && ('\t' != c) && ('\n' != c) && ('\r' != c) ) break;
    in_u->pos_z++;
  }
}

static c3_ys
_json_peek(_json_in* in_u)
{
  _json_skip(in_u);
  return ( in_u->pos_z < in_u->len_z ) ? in_u->txt_c[in_u->pos_z] : -1;
}

static c3_o
_json_lit(_json_in* in_u, const c3_c* lit_c)
{
  c3_z len_z = strlen(lit_c);

  if ( (in_u->len_z - in_u->pos_z) < len_z
    || memcmp(in_u->txt_c + in_u->pos_z, lit_c, len_z) )
  {
    return c3n;
  }
  in_u->pos_z += len_z;
  return c3y;
}

static json*
_json_new(json_type typ_e)
{
  json* jon_u = c3_calloc(sizeof(*jon_u));
  jon_u->typ_e = typ_e;
  return jon_u;
}

static c3_ys
_json_hex(c3_c c)
{
  if ( (c >= '0') && (c <= '9') ) return c - '0';
  if ( (c >= 'a') && (c <= 'f') ) return 10 + (c - 'a');
  if ( (c >= 'A') && (c <= 'F') ) return 10 + (c - 'A');
  return -1;
}

/* _json_string(): parse a string at the opening quote; NUL-terminated
*/
static c3_c*
_json_string(_json_in* in_u, c3_z* out_z)
{
  c3_c* out_c;
  c3_z  n_z = 0;

  in_u->pos_z++;   //  '"'

  //  unescaping never grows the text
  //
  out_c = c3_malloc(in_u->len_z - in_u->pos_z + 1);

  while ( in_u->pos_z < in_u->len_z ) {
    c3_c c = in_u->txt_c[in_u->pos_z++];

    if ( '"' == c ) {
      out_c[n_z] = 0;
      *out_z = n_z;
      return out_c;
    }
    if ( (c3_y)c < 0x20 ) {
      break;
    }
    if ( '\\' != c ) {
      out_c[n_z++] = c;
      continue;
    }
    if ( in_u->pos_z >= in_u->len_z ) {
      break;
    }

    c = in_u->txt_c[in_u->pos_z++];
    switch ( c ) {
      case '"':  out_c[n_z++] = '"';  break;
      case '\\': out_c[n_z++] = '\\'; break;
      case '/':  out_c[n_z++] = '/';  break;
      case 'b':  out_c[n_z++] = '\b'; break;
      case 'f':  out_c[n_z++] = '\f'; break;
      case 'n':  out_c[n_z++] = '\n'; break;
      case 'r':  out_c[n_z++] = '\r'; break;
      case 't':  out_c[n_z++] = '\t'; break;
      case 'u': {
        c3_h cod_h = 0;

        if ( (in_u->len_z - in_u->pos_z) < 4 ) goto fail;
        for ( c3_w i = 0; i < 4; i++ ) {
          c3_ys h = _json_hex(in_u->txt_c[in_u->pos_z++]);
          if ( h < 0 ) goto fail;
          cod_h = (cod_h << 4) | (c3_h)h;
        }

        //  BMP only, surrogates rejected; \uXXXX is 6 bytes of input
        //  and at most 3 of output
        //
        if ( (cod_h >= 0xd800) && (cod_h <= 0xdfff) ) goto fail;
        if ( cod_h < 0x80 ) {
          out_c[n_z++] = (c3_c)cod_h;
        }
        else if ( cod_h < 0x800 ) {
          out_c[n_z++] = (c3_c)(0xc0 | (cod_h >> 6));
          out_c[n_z++] = (c3_c)(0x80 | (cod_h & 0x3f));
        }
        else {
          out_c[n_z++] = (c3_c)(0xe0 | (cod_h >> 12));
          out_c[n_z++] = (c3_c)(0x80 | ((cod_h >> 6) & 0x3f));
          out_c[n_z++] = (c3_c)(0x80 | (cod_h & 0x3f));
        }
      } break;
      default: goto fail;
    }
  }

fail:
  c3_free(out_c);
  return 0;
}

static json*
_json_number(_json_in* in_u)
{
  c3_z  sat_z = in_u->pos_z;
  json* jon_u;

  while ( in_u->pos_z < in_u->len_z ) {
    c3_c c = in_u->txt_c[in_u->pos_z];
    if ( !(isdigit((c3_y)c) || ('-' == c) || ('+' == c)
         || ('.' == c) || ('e' == c) || ('E' == c)) )
    {
      break;
    }
    in_u->pos_z++;
  }

  if ( sat_z == in_u->pos_z ) {
    return 0;
  }

  jon_u = _json_new(JSON_NUM);
  jon_u->len_z = in_u->pos_z - sat_z;
  jon_u->str_c = c3_malloc(jon_u->len_z + 1);
  memcpy(jon_u->str_c, in_u->txt_c + sat_z, jon_u->len_z);
  jon_u->str_c[jon_u->len_z] = 0;
  return jon_u;
}

static void
_json_push(json* jon_u, c3_c* key_c, json* val_u)
{
  c3_z n_z = jon_u->len_z + 1;

  jon_u->val_u = c3_realloc(jon_u->val_u, n_z * sizeof(json*));
  jon_u->val_u[n_z - 1] = val_u;
  if ( JSON_OBJ == jon_u->typ_e ) {
    jon_u->key_c = c3_realloc(jon_u->key_c, n_z * sizeof(c3_c*));
    jon_u->key_c[n_z - 1] = key_c;
  }
  jon_u->len_z = n_z;
}

static json*
_json_container(_json_in* in_u, c3_w dep_w, c3_o obj_o)
{
  json* jon_u = _json_new(( c3y == obj_o ) ? JSON_OBJ : JSON_ARR);
  c3_c  end_c = ( c3y == obj_o ) ? '}' : ']';

  in_u->pos_z++;   //  '{' or '['

  if ( end_c == _json_peek(in_u) ) {
    in_u->pos_z++;
    return jon_u;
  }

  while ( 1 ) {
    c3_c* key_c = 0;
    json* val_u;
    c3_ys c;

    if ( c3y == obj_o ) {
      c3_z len_z;

      if ( '"' != _json_peek(in_u) ) goto fail;
      if ( !(key_c = _json_string(in_u, &len_z)) ) goto fail;
      if ( ':' != _json_peek(in_u) ) { c3_free(key_c); goto fail; }
      in_u->pos_z++;
    }

    if ( !(val_u = _json_value(in_u, dep_w + 1)) ) {
      c3_free(key_c);
      goto fail;
    }
    _json_push(jon_u, key_c, val_u);

    c = _json_peek(in_u);
    in_u->pos_z++;
    if ( end_c == c ) return jon_u;
    if ( ',' != c ) goto fail;
  }

fail:
  json_free(jon_u);
  return 0;
}

static json*
_json_value(_json_in* in_u, c3_w dep_w)
{
  c3_ys c;

  if ( dep_w > JSON_DEPTH ) {
    return 0;
  }

  c = _json_peek(in_u);

  switch ( c ) {
    case '{': return _json_container(in_u, dep_w, c3y);
    case '[': return _json_container(in_u, dep_w, c3n);

    case '"': {
      json* jon_u = _json_new(JSON_STR);
      if ( !(jon_u->str_c = _json_string(in_u, &jon_u->len_z)) ) {
        c3_free(jon_u);
        return 0;
      }
      return jon_u;
    }

    case 't': case 'f': {
      json* jon_u;
      c3_o  bol_o = ( 't' == c ) ? c3y : c3n;
      if ( c3n == _json_lit(in_u, ( c3y == bol_o ) ? "true" : "false") ) {
        return 0;
      }
      jon_u = _json_new(JSON_BOOL);
      jon_u->bol_o = bol_o;
      return jon_u;
    }

    case 'n': {
      if ( c3n == _json_lit(in_u, "null") ) return 0;
      return _json_new(JSON_NULL);
    }

    default: return _json_number(in_u);
  }
}

json*
json_parse(c3_z len_z, const c3_c* txt_c)
{
  _json_in in_u = { .txt_c = txt_c, .len_z = len_z, .pos_z = 0 };
  json*    jon_u = _json_value(&in_u, 0);

  if ( jon_u && (-1 != _json_peek(&in_u)) ) {
    json_free(jon_u);
    return 0;
  }
  return jon_u;
}

void
json_free(json* jon_u)
{
  if ( !jon_u ) return;

  for ( c3_z i_z = 0;
        (JSON_ARR == jon_u->typ_e || JSON_OBJ == jon_u->typ_e)
        && i_z < jon_u->len_z;
        i_z++ )
  {
    json_free(jon_u->val_u[i_z]);
    if ( jon_u->key_c ) c3_free(jon_u->key_c[i_z]);
  }
  c3_free(jon_u->val_u);
  c3_free(jon_u->key_c);
  c3_free(jon_u->str_c);
  c3_free(jon_u);
}

json*
json_get(const json* jon_u, const c3_c* key_c)
{
  if ( !jon_u || (JSON_OBJ != jon_u->typ_e) ) {
    return 0;
  }
  for ( c3_z i_z = 0; i_z < jon_u->len_z; i_z++ ) {
    if ( 0 == strcmp(key_c, jon_u->key_c[i_z]) ) {
      return jon_u->val_u[i_z];
    }
  }
  return 0;
}

const c3_c*
json_str(const json* jon_u)
{
  return ( jon_u && (JSON_STR == jon_u->typ_e) ) ? jon_u->str_c : 0;
}

c3_o
json_u64(const json* jon_u, c3_d* out_d)
{
  const c3_c* txt_c;
  c3_d        val_d = 0;

  if ( !jon_u
    || ((JSON_NUM != jon_u->typ_e) && (JSON_STR != jon_u->typ_e)) )
  {
    return c3n;
  }

  txt_c = jon_u->str_c;
  if ( !*txt_c ) {
    return c3n;
  }
  for ( ; *txt_c; txt_c++ ) {
    if ( !isdigit((c3_y)*txt_c) ) return c3n;
    if ( val_d > (UINT64_MAX - 9) / 10 ) return c3n;
    val_d = (val_d * 10) + (c3_d)(*txt_c - '0');
  }

  *out_d = val_d;
  return c3y;
}

/// @file

#include "compat.h"
#include "mesa/gall.h"

static mesa_noun
_ge(const char* agent)
{
  return mesa_cell(mesa_atom_cord("ge"),
                   mesa_cell(mesa_atom_cord(agent), mesa_atom_u64(0)));
}

static mesa_err
_request(mesa_engine* eng, mesa_ship her, uint64_t bone, const char* agent,
         mesa_noun req)
{
  //  [%0 ames-request]
  //
  return mesa_flow_plea(eng, her, bone, mesa_atom_cord("g"), _ge(agent),
                        mesa_cell(mesa_atom_u64(0), req));
}

mesa_err
mesa_gall_poke(mesa_engine* eng, mesa_ship her, uint64_t bone,
               const char* agent, const char* mark, mesa_noun noun)
{
  return _request(eng, her, bone, agent,
                  mesa_trel(mesa_atom_cord("m"), mesa_atom_cord(mark), noun));
}

mesa_err
mesa_gall_watch(mesa_engine* eng, mesa_ship her, uint64_t bone,
                const char* agent, mesa_noun path)
{
  return _request(eng, her, bone, agent, mesa_cell(mesa_atom_cord("s"), path));
}

mesa_err
mesa_gall_leave(mesa_engine* eng, mesa_ship her, uint64_t bone,
                const char* agent)
{
  return _request(eng, her, bone, agent,
                  mesa_cell(mesa_atom_cord("u"), mesa_atom_u64(0)));
}

static c3_o
_tag(mesa_noun som, const char* tag_c)
{
  char* tas_c;
  c3_o  ret_o;

  if ( (MESA_NONE == som) || !mesa_is_atom(som) || (mesa_met3(som) > 32) ) {
    return c3n;
  }
  tas_c = mesa_atom_get_cord(som);
  ret_o = __(0 == strcmp(tas_c, tag_c));
  c3_free(tas_c);
  return ret_o;
}

mesa_gall_response_kind
mesa_gall_response(mesa_noun boon, mesa_noun* mark, mesa_noun* noun)
{
  if ( !mesa_is_cell(boon) ) {
    return MESA_GALL_BAD;
  }
  if ( (c3y == _tag(mesa_head(boon), "d")) && mesa_is_cell(mesa_tail(boon)) ) {
    *mark = mesa_at(6, boon);
    *noun = mesa_at(7, boon);
    return MESA_GALL_FACT;
  }
  if ( c3y == _tag(mesa_head(boon), "x") ) {
    return MESA_GALL_KICK;
  }
  return MESA_GALL_BAD;
}

mesa_gall_request_kind
mesa_gall_request_parse(mesa_noun vane, mesa_noun path, mesa_noun payload,
                        mesa_gall_request* req)
{
  mesa_noun pre, age, bod;

  memset(req, 0, sizeof(*req));
  req->mark = req->noun = req->path = MESA_NONE;

  if (  (c3n == _tag(vane, "g"))
     || !mesa_is_cell(path)
     || !mesa_is_cell(mesa_tail(path))
     || (0 != mesa_tail(mesa_tail(path))) )
  {
    return MESA_GALL_NONE;
  }
  pre = mesa_head(path);
  age = mesa_head(mesa_tail(path));

  if ( (c3y == _tag(pre, "gf")) || (c3y == _tag(pre, "gk")) ) {
    return (req->kind = MESA_GALL_SYSTEM);
  }
  if (  ((c3n == _tag(pre, "ge")) && (c3n == _tag(pre, "gp")))
     || !mesa_is_atom(age) || (mesa_met3(age) >= sizeof(req->agent)) )
  {
    return MESA_GALL_NONE;
  }
  mesa_atom_get_bytes(age, mesa_met3(age), (uint8_t*)req->agent);

  //  [%0 ames-request]
  //
  if (  !mesa_is_cell(payload)
     || (MESA_NONE == mesa_at(2, payload)) || !mesa_is_atom(mesa_head(payload))
     || (0 != mesa_met3(mesa_head(payload)))
     || !mesa_is_cell(bod = mesa_tail(payload)) )
  {
    return MESA_GALL_NONE;
  }

  if ( (c3y == _tag(mesa_head(bod), "m")) && mesa_is_cell(mesa_tail(bod)) ) {
    req->mark = mesa_at(6, bod);
    req->noun = mesa_at(7, bod);
    return (req->kind = MESA_GALL_POKE);
  }
  if ( c3y == _tag(mesa_head(bod), "s") ) {
    req->path = mesa_tail(bod);
    return (req->kind = MESA_GALL_WATCH);
  }
  if ( (c3y == _tag(mesa_head(bod), "l")) && mesa_is_cell(mesa_tail(bod)) ) {
    req->mark = mesa_at(6, bod);
    req->path = mesa_at(7, bod);
    return (req->kind = MESA_GALL_WATCH_AS);
  }
  if ( c3y == _tag(mesa_head(bod), "u") ) {
    return (req->kind = MESA_GALL_LEAVE);
  }
  return MESA_GALL_NONE;
}

mesa_err
mesa_gall_fact(mesa_engine* eng, mesa_ship her, uint64_t bone,
               const char* mark, mesa_noun noun)
{
  return mesa_flow_boon(eng, her, bone,
                        mesa_trel(mesa_atom_cord("d"), mesa_atom_cord(mark), noun));
}

mesa_err
mesa_gall_kick(mesa_engine* eng, mesa_ship her, uint64_t bone)
{
  return mesa_flow_boon(eng, her, bone,
                        mesa_cell(mesa_atom_cord("x"), mesa_atom_u64(0)));
}

/* _path(): a path noun from "/a/b" (empty segments allowed: "//1")
*/
static mesa_noun
_path(const char* pat_c, size_t len_z)
{
  const char* end_c;

  if ( !len_z || ('/' != pat_c[0]) ) {
    return mesa_atom_u64(0);
  }
  pat_c++, len_z--;
  if ( !len_z ) {
    return mesa_atom_u64(0);
  }
  end_c = memchr(pat_c, '/', len_z);
  {
    size_t    seg_z = end_c ? (size_t)(end_c - pat_c) : len_z;
    mesa_noun seg   = mesa_atom_bytes(seg_z, (const uint8_t*)pat_c);
    return mesa_cell(seg, end_c ? _path(end_c, len_z - seg_z)
                                : mesa_atom_u64(0));
  }
}

mesa_err
mesa_gall_key(mesa_engine* eng, mesa_ship her, uint64_t bone,
              const char* path)
{
  //  /g/x/<case>/<agent>/<rest>: [%0 rest] on /gk/<agent>
  //
  const char* seg_c[4];
  const char* cur_c = path;
  char        dap_c[64];
  size_t      dap_z;

  if ( !path || ('/' != path[0]) || strncmp(path, "/g/x/", 5) ) {
    return MESA_E_ARG;
  }
  for ( int i = 0; i < 4; i++ ) {
    seg_c[i] = cur_c ? cur_c + 1 : 0;
    cur_c = cur_c ? strchr(cur_c + 1, '/') : 0;
  }
  if ( !cur_c || !seg_c[3] ) {
    return MESA_E_ARG;
  }
  dap_z = (size_t)(cur_c - seg_c[3]);
  if ( !dap_z || (dap_z >= sizeof(dap_c)) ) {
    return MESA_E_ARG;
  }
  memcpy(dap_c, seg_c[3], dap_z);
  dap_c[dap_z] = 0;

  return mesa_flow_plea(eng, her, bone, mesa_atom_cord("g"),
                        mesa_cell(mesa_atom_cord("gk"),
                                  mesa_cell(mesa_atom_cord(dap_c),
                                            mesa_atom_u64(0))),
                        mesa_cell(mesa_atom_u64(0),
                                  _path(cur_c, strlen(cur_c))));
}

bool
mesa_gall_key_parse(mesa_noun boon, uint64_t* kid, uint8_t key[32])
{
  //  [%0 ~ coop hutch=[rev idx key]]
  //
  mesa_noun hut = mesa_at(15, boon);
  mesa_noun idx, ky;

  if (  !mesa_is_cell(boon) || (0 != mesa_head(boon))
     || (MESA_NONE == mesa_at(6, boon)) || (0 != mesa_at(6, boon))
     || (MESA_NONE == hut) || !mesa_is_cell(hut)
     || (MESA_NONE == (idx = mesa_at(6, hut)))
     || (MESA_NONE == (ky  = mesa_at(7, hut)))
     || !mesa_is_atom(ky) || (mesa_met3(ky) > 64)
     || !mesa_atom_get_u64(idx, kid) || !*kid )
  {
    return false;
  }
  //  64-byte keys (%plug) are used through their first 32 bytes
  //
  mesa_atom_get_bytes(ky, 32, key);
  return true;
}

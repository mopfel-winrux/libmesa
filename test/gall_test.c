/// @file

#include "compat.h"
#include "mesa/gall.h"

static c3_i ret_i = 0;

#define expect(c, m) do { if ( !(c) ) { fprintf(stderr, "gall: %s\r\n", m); ret_i = 1; } } while (0)

static mesa_noun
_path2(const char* a, const char* b)
{
  return mesa_cell(mesa_atom_cord(a), mesa_cell(mesa_atom_cord(b), mesa_atom_u64(0)));
}

int
main(int argc, char* argv[])
{
  mesa_gall_request req;
  mesa_noun         van = mesa_atom_cord("g");
  mesa_noun         pax = _path2("ge", "my-agent");
  mesa_noun         pay, mar, nou;

  //  [%0 %m %noun 42]
  //
  pay = mesa_cell(mesa_atom_u64(0),
                  mesa_trel(mesa_atom_cord("m"), mesa_atom_cord("noun"), mesa_atom_u64(42)));
  expect(MESA_GALL_POKE == mesa_gall_request_parse(van, pax, pay, &req), "poke");
  expect(!strcmp(req.agent, "my-agent"), "agent");
  {
    uint64_t v = 0;
    mesa_atom_get_u64(req.noun, &v);
    expect(42 == v, "poke noun");
  }
  mesa_unref(pay);

  //  [%0 %s /updates]
  //
  pay = mesa_cell(mesa_atom_u64(0),
                  mesa_cell(mesa_atom_cord("s"),
                            mesa_cell(mesa_atom_cord("updates"), mesa_atom_u64(0))));
  expect(MESA_GALL_WATCH == mesa_gall_request_parse(van, pax, pay, &req), "watch");
  mesa_unref(pay);

  //  [%0 %u ~]
  //
  pay = mesa_cell(mesa_atom_u64(0), mesa_cell(mesa_atom_cord("u"), mesa_atom_u64(0)));
  expect(MESA_GALL_LEAVE == mesa_gall_request_parse(van, pax, pay, &req), "leave");
  mesa_unref(pay);

  //  gall's own flows
  //
  {
    mesa_noun sys = _path2("gf", "my-agent");
    pay = mesa_cell(mesa_atom_u64(0), mesa_atom_u64(0));
    expect(MESA_GALL_SYSTEM == mesa_gall_request_parse(van, sys, pay, &req), "flub flow");
    mesa_unref(pay);
    mesa_unref(sys);
  }

  //  junk
  //
  pay = mesa_atom_u64(7);
  expect(MESA_GALL_NONE == mesa_gall_request_parse(van, pax, pay, &req), "junk");
  mesa_unref(pay);

  //  responses
  //
  pay = mesa_trel(mesa_atom_cord("d"), mesa_atom_cord("noun"), mesa_atom_u64(5));
  expect(MESA_GALL_FACT == mesa_gall_response(pay, &mar, &nou), "fact");
  mesa_unref(pay);
  pay = mesa_cell(mesa_atom_cord("x"), mesa_atom_u64(0));
  expect(MESA_GALL_KICK == mesa_gall_response(pay, &mar, &nou), "kick");
  mesa_unref(pay);

  mesa_unref(van);
  mesa_unref(pax);
  fprintf(stderr, ret_i ? "gall: failed\r\n" : "gall ok\r\n");
  return ret_i;
}

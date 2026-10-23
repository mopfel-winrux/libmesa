//  test/install/core.c: links against an installed libmesa (see check.sh)

#include <mesa/engine.h>
#include <stdio.h>
#include <stdlib.h>

int
main(void)
{
  mesa_config  cfg = {0};
  mesa_engine* eng;
  mesa_ship    who;
  char*        txt;

  if ( !mesa_ship_parse("~zod", &who) ) return 1;
  mesa_ident_fake(who, &cfg.ident);
  cfg.store    = mesa_store_memory();
  cfg.breached = true;
  cfg.fake     = true;

  if ( MESA_OK != mesa_engine_open(&cfg, &eng) ) return 1;
  txt = mesa_ship_render(mesa_engine_ship(eng));
  printf("core: %s\n", txt);
  free(txt);
  mesa_engine_close(eng);
  mesa_store_free(cfg.store);
  return 0;
}

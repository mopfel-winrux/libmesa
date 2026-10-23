//  test/install/host.c: links against an installed libmesa-host (see
//  check.sh); opens a fake ~nec on an ephemeral port and closes it

#include <mesa/host.h>
#include <stdio.h>
#include <stdlib.h>
#include <uv.h>

int
main(void)
{
  mesa_host_config cfg = {0};
  mesa_host*       hos;
  mesa_ship        who;
  char*            txt;
  uv_loop_t        lop;

  if ( !mesa_ship_parse("~nec", &who) ) return 1;
  mesa_ident_fake(who, &cfg.engine.ident);
  cfg.engine.store    = mesa_store_memory();
  cfg.engine.breached = true;
  cfg.engine.no_ping  = true;
  cfg.fake            = true;
  cfg.port            = 0;
  uv_loop_init(&lop);
  cfg.loop            = &lop;

  if ( 0 != mesa_host_open(&cfg, &hos) ) return 1;
  txt = mesa_ship_render(mesa_engine_ship(mesa_host_engine(hos)));
  printf("host: %s\n", txt);
  free(txt);
  mesa_host_close(hos);
  uv_loop_close(&lop);
  mesa_store_free(cfg.engine.store);
  return 0;
}

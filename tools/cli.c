/// @file
///
/// Identity and store options for the command-line tools: see cli.h.

#include "cli.h"

#include "mesa/ident.h"

#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/random.h>
#include <unistd.h>

static int
_read_key(const char* pat, mesa_ident* ide)
{
  FILE*  f = fopen(pat, "r");
  char   key[8192] = {0};
  size_t n;

  if ( !f ) return 0;
  n = fread(key, 1, sizeof(key) - 1, f);
  fclose(f);
  return n && (mesa_keyfile_parse(key, ide, 1) > 0);
}

/* comets need a star that's online to sponsor them
*/
static int
_mine(mesa_ident* ide)
{
  size_t    siz;
  uint8_t*  buf = mesa_host_get("https://bootstrap.urbit.org/comet-stars.jam", &siz);
  mesa_noun lis;
  mesa_ship sta[4096];
  size_t    len = 0;
  uint8_t   eny[64];

  if ( !buf ) {
    fprintf(stderr, "can't download comet-stars.jam\n");
    return 0;
  }
  lis = mesa_cue_bytes(siz, buf);
  free(buf);
  if ( MESA_NONE == lis ) return 0;
  for ( mesa_noun i = lis; mesa_is_cell(i) && len < 4096; i = mesa_tail(i) ) {
    uint64_t s;
    if ( mesa_atom_get_u64(mesa_head(i), &s) ) {
      sta[len++] = (mesa_ship){ .lo = s, .hi = 0 };
    }
  }
  mesa_unref(lis);

  if ( 64 != getrandom(eny, 64, 0) ) return 0;
  fprintf(stderr, "mining a comet under %zu stars...\n", len);
  return mesa_comet_mine(len, sta, eny, UINT64_MAX, ide);
}

int
cli_option(cli* cli_u, int argc, char* argv[], int* i)
{
  char* a = argv[*i];

  if ( !strcmp(a, "-F") && *i + 1 < argc ) {
    mesa_ship who;
    if ( !mesa_ship_parse(argv[++*i], &who) ) return 0;
    mesa_ident_fake(who, &cli_u->ide);
    cli_u->cfg.fake = true;
    cli_u->cfg.engine.breached = true;
    cli_u->hav = 1;
  }
  else if ( !strcmp(a, "-k") && *i + 1 < argc ) {
    if ( !_read_key(argv[++*i], &cli_u->ide) ) {
      fprintf(stderr, "bad keyfile %s\n", argv[*i]);
      exit(2);
    }
    cli_u->hav = 1;
  }
  else if ( !strcmp(a, "-c") ) { cli_u->hav = 2; }
  else if ( !strcmp(a, "-s") && *i + 1 < argc ) { cli_u->db = argv[++*i]; }
  else if ( !strcmp(a, "-b") ) { cli_u->cfg.engine.breached = true; }
  else if ( !strcmp(a, "-p") && *i + 1 < argc ) {
    cli_u->cfg.port = (uint16_t)atoi(argv[++*i]);
  }
  else return 0;
  return 1;
}

int
cli_open(cli* cli_u)
{
  if ( !cli_u->hav ) {
    fprintf(stderr, "no identity: -F, -k or -c\n");
    return 0;
  }

  if ( 2 == cli_u->hav ) {
    //  a comet's keys live next to its state: reuse them if we have them
    //
    char  kef[1024];
    FILE* f;
    snprintf(kef, sizeof(kef), "%s.key", cli_u->db ? cli_u->db : "comet");
    if ( (f = fopen(kef, "r")) ) {
      fclose(f);
      if ( !_read_key(kef, &cli_u->ide) ) {
        fprintf(stderr, "bad %s\n", kef);
        return 0;
      }
    }
    else {
      char* str;
      if ( !_mine(&cli_u->ide) ) {
        fprintf(stderr, "mining failed\n");
        return 0;
      }
      str = mesa_keyfile_render(&cli_u->ide);
      {
        //  the keys are secret: owner-only, and never over another file
        //
        int fd = open(kef, O_WRONLY | O_CREAT | O_EXCL, 0600);
        if ( (fd >= 0) && (f = fdopen(fd, "w")) ) { fprintf(f, "%s\n", str); fclose(f); }
        else if ( fd >= 0 ) close(fd);
      }
      free(str);
    }
    {
      char* who = mesa_ship_render(cli_u->ide.who);
      fprintf(stderr, "we are %s\n", who);
      free(who);
    }
  }

  if ( cli_u->db ) {
    const char* e;
    if ( !(cli_u->cfg.engine.store = mesa_store_sqlite(cli_u->db, &e)) ) {
      fprintf(stderr, "%s: %s\n", cli_u->db, e);
      return 0;
    }
  }
  else {
    cli_u->cfg.engine.store = mesa_store_memory();
    cli_u->cfg.engine.breached = true;
  }
  cli_u->cfg.engine.ident = cli_u->ide;
  return 1;
}

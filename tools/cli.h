/// @file
///
/// What the command-line tools share: choosing an identity and a store.
///
///   -F ~ship        fake ship (to talk to `urbit -F` ships on localhost)
///   -k keyfile      a planet, star, galaxy or moon keyfile (@uw)
///   -c              mine a fresh comet (keyfile saved to <state>.key)
///   -s state.db     persistent state (SQLite); default: in memory
///   -b              this identity breached (allow an empty state)
///   -p port         UDP port to bind

#ifndef MESA_CLI_H
#define MESA_CLI_H

#include "mesa/host.h"

typedef struct cli {
  mesa_host_config cfg;
  mesa_ident       ide;
  int              hav;    //  1: -F or -k, 2: -c
  const char*      db;
} cli;

#define CLI_USAGE "(-F ~ship | -k keyfile | -c) [-s db] [-b] [-p port]"

/* cli_option(): take argv[*i] (and its argument) if it's one of ours
*/
int cli_option(cli* cli_u, int argc, char* argv[], int* i);

/* cli_open(): settle the identity (mining a comet if needed) and open
**             the store; prints why and returns 0 on failure
*/
int cli_open(cli* cli_u);

#endif /* ifndef MESA_CLI_H */

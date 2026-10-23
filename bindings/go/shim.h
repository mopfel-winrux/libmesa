#ifndef MESA_GO_SHIM_H
#define MESA_GO_SHIM_H

#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "mesa/azimuth.h"
#include "mesa/engine.h"
#include "mesa/gall.h"
#include "mesa/ident.h"
#include "mesa/noun.h"
#include "mesa/store.h"

void        mesa_go_hooks(mesa_hooks* hok, uintptr_t ctx);
mesa_store* mesa_go_store(uintptr_t ctx, int sav);
uint64_t    mesa_go_thread(void);   //  this OS thread (pthread_self)
int         mesa_go_scan_row(void* fun, void* arg, const uint8_t* key,
                             size_t key_len, const uint8_t* val,
                             size_t val_len);

#endif

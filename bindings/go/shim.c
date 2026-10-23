//  C side of the Go binding: libmesa's hooks and store callbacks take
//  structs by value and a void* context, which cgo exports can't; these
//  trampolines forward each call to a Go export with a handle.

#include "shim.h"

#include <pthread.h>

#include "_cgo_export.h"

//  engine hooks

static void
_send(void* ctx, mesa_lane lane, const uint8_t* buf, size_t len)
{
  goSend((uintptr_t)ctx, &lane, (uint8_t*)buf, len);
}

static void
_need_point(void* ctx, mesa_ship who)
{
  goNeedPoint((uintptr_t)ctx, &who);
}

static mesa_plea_result
_plea(void* ctx, mesa_ship her, uint64_t bone, mesa_noun vane, mesa_noun path,
      mesa_noun payload, mesa_noun* error)
{
  return (mesa_plea_result)goPlea((uintptr_t)ctx, &her, bone, vane, path,
                                  payload, error);
}

static void
_boon(void* ctx, mesa_ship her, uint64_t bone, mesa_noun payload)
{
  goBoon((uintptr_t)ctx, &her, bone, payload);
}

static void
_done(void* ctx, mesa_ship her, uint64_t bone, mesa_noun error)
{
  goDone((uintptr_t)ctx, &her, bone, error);
}

static void
_corked(void* ctx, mesa_ship her, uint64_t bone)
{
  goCorked((uintptr_t)ctx, &her, bone);
}

static void
_sage(void* ctx, mesa_ship her, const char* path, mesa_noun gage)
{
  goSage((uintptr_t)ctx, &her, (char*)path, gage);
}

static mesa_noun
_peek(void* ctx, const mesa_ship* her, const char* path)
{
  return goPeek((uintptr_t)ctx, (mesa_ship*)her, (char*)path);
}

static void
_breach(void* ctx, mesa_ship her)
{
  goBreach((uintptr_t)ctx, &her);
}

void
mesa_go_hooks(mesa_hooks* hok, uintptr_t ctx)
{
  hok->ctx        = (void*)ctx;
  hok->send       = _send;
  hok->need_point = _need_point;
  hok->plea       = _plea;
  hok->boon       = _boon;
  hok->done       = _done;
  hok->corked     = _corked;
  hok->sage       = _sage;
  hok->peek       = _peek;
  hok->breach     = _breach;
}

//  store callbacks

static int _begin(void* ctx)    { return goStoreOp((uintptr_t)ctx, 0); }
static int _commit(void* ctx)   { return goStoreOp((uintptr_t)ctx, 1); }
static int _rollback(void* ctx) { return goStoreOp((uintptr_t)ctx, 2); }
static int _mark(void* ctx)     { return goStoreOp((uintptr_t)ctx, 3); }
static int _rewind(void* ctx)   { return goStoreOp((uintptr_t)ctx, 4); }
static int _release(void* ctx)  { return goStoreOp((uintptr_t)ctx, 5); }

static int
_get(void* ctx, const uint8_t* key, size_t key_len, uint8_t** val,
     size_t* val_len)
{
  return goStoreGet((uintptr_t)ctx, (uint8_t*)key, key_len, val, val_len);
}

static int
_put(void* ctx, const uint8_t* key, size_t key_len, const uint8_t* val,
     size_t val_len)
{
  return goStorePut((uintptr_t)ctx, (uint8_t*)key, key_len, (uint8_t*)val,
                    val_len);
}

static int
_del(void* ctx, const uint8_t* key, size_t key_len)
{
  return goStoreDel((uintptr_t)ctx, (uint8_t*)key, key_len);
}

static int
_scan(void* ctx, const uint8_t* pre, size_t pre_len, mesa_store_scan_f fun,
      void* arg)
{
  return goStoreScan((uintptr_t)ctx, (uint8_t*)pre, pre_len, (void*)fun, arg);
}

static void
_close(void* ctx)
{
}

mesa_store*
mesa_go_store(uintptr_t ctx, int sav)
{
  mesa_store* sto = calloc(1, sizeof(*sto));

  sto->ctx      = (void*)ctx;
  sto->begin    = _begin;
  sto->commit   = _commit;
  sto->rollback = _rollback;
  sto->mark     = sav ? _mark : 0;
  sto->rewind   = sav ? _rewind : 0;
  sto->release  = sav ? _release : 0;
  sto->get      = _get;
  sto->put      = _put;
  sto->del      = _del;
  sto->scan     = _scan;
  sto->close    = _close;
  return sto;
}

int
mesa_go_scan_row(void* fun, void* arg, const uint8_t* key, size_t key_len,
                 const uint8_t* val, size_t val_len)
{
  return ((mesa_store_scan_f)fun)(arg, key, key_len, val, val_len);
}

uint64_t
mesa_go_thread(void)
{
  return (uint64_t)(uintptr_t)pthread_self();
}

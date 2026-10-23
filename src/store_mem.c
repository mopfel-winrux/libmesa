/// @file
///
/// A volatile, in-memory mesa_store: a sorted array of rows, with an
/// undo journal for rollback and savepoints.

#include "compat.h"
#include "mesa/store.h"

typedef struct {
  c3_y* key_y;
  c3_z  key_z;
  c3_y* val_y;
  c3_z  val_z;
} _row;

typedef struct {
  _row  row_u;      //  the key, and its old value (val_y 0: was absent)
  c3_o  had_o;
} _undo;

typedef struct {
  mesa_store sto_u;
  _row*      row_u;
  c3_z       len_z, cap_z;
  _undo*     und_u;
  c3_z       unl_z, unc_z;
  c3_z       mar_z[64];   //  savepoint stack (journal lengths)
  c3_w       man_w;
  c3_o       txn_o;
} _mem;

static c3_i
_cmp(const c3_y* a_y, c3_z a_z, const c3_y* b_y, c3_z b_z)
{
  c3_i r = memcmp(a_y, b_y, c3_min(a_z, b_z));
  if ( r ) return r;
  return ( a_z < b_z ) ? -1 : ( a_z > b_z ) ? 1 : 0;
}

/* _find(): index of [key], or where it would go; *hit set if present
*/
static c3_z
_find(_mem* mem_u, const c3_y* key_y, c3_z key_z, c3_o* hit_o)
{
  c3_z lo = 0, hi = mem_u->len_z;

  while ( lo < hi ) {
    c3_z mid = (lo + hi) / 2;
    c3_i r   = _cmp(mem_u->row_u[mid].key_y, mem_u->row_u[mid].key_z,
                    key_y, key_z);
    if ( 0 == r ) {
      *hit_o = c3y;
      return mid;
    }
    if ( r < 0 ) lo = mid + 1;
    else         hi = mid;
  }
  *hit_o = c3n;
  return lo;
}

static c3_y*
_mem_dup(const c3_y* buf_y, c3_z len_z)
{
  c3_y* out_y = c3_malloc(c3_max(len_z, 1));
  memcpy(out_y, buf_y, len_z);
  return out_y;
}

static void
_journal(_mem* mem_u, const c3_y* key_y, c3_z key_z)
{
  c3_o  hit_o;
  c3_z  i_z;
  _undo und_u = {0};

  if ( c3n == mem_u->txn_o ) {
    return;
  }

  i_z = _find(mem_u, key_y, key_z, &hit_o);
  und_u.row_u.key_y = _mem_dup(key_y, key_z);
  und_u.row_u.key_z = key_z;
  und_u.had_o = hit_o;
  if ( c3y == hit_o ) {
    und_u.row_u.val_y = _mem_dup(mem_u->row_u[i_z].val_y, mem_u->row_u[i_z].val_z);
    und_u.row_u.val_z = mem_u->row_u[i_z].val_z;
  }

  if ( mem_u->unl_z == mem_u->unc_z ) {
    mem_u->unc_z = c3_max(16, mem_u->unc_z * 2);
    mem_u->und_u = c3_realloc(mem_u->und_u, mem_u->unc_z * sizeof(_undo));
  }
  mem_u->und_u[mem_u->unl_z++] = und_u;
}

static void
_set(_mem* mem_u, const c3_y* key_y, c3_z key_z,
     const c3_y* val_y, c3_z val_z)
{
  c3_o hit_o;
  c3_z i_z = _find(mem_u, key_y, key_z, &hit_o);

  if ( c3y == hit_o ) {
    c3_free(mem_u->row_u[i_z].val_y);
    mem_u->row_u[i_z].val_y = _mem_dup(val_y, val_z);
    mem_u->row_u[i_z].val_z = val_z;
    return;
  }

  if ( mem_u->len_z == mem_u->cap_z ) {
    mem_u->cap_z = c3_max(16, mem_u->cap_z * 2);
    mem_u->row_u = c3_realloc(mem_u->row_u, mem_u->cap_z * sizeof(_row));
  }
  memmove(mem_u->row_u + i_z + 1, mem_u->row_u + i_z,
          (mem_u->len_z - i_z) * sizeof(_row));
  mem_u->row_u[i_z].key_y = _mem_dup(key_y, key_z);
  mem_u->row_u[i_z].key_z = key_z;
  mem_u->row_u[i_z].val_y = _mem_dup(val_y, val_z);
  mem_u->row_u[i_z].val_z = val_z;
  mem_u->len_z++;
}

static void
_unset(_mem* mem_u, const c3_y* key_y, c3_z key_z)
{
  c3_o hit_o;
  c3_z i_z = _find(mem_u, key_y, key_z, &hit_o);

  if ( c3n == hit_o ) {
    return;
  }
  c3_free(mem_u->row_u[i_z].key_y);
  c3_free(mem_u->row_u[i_z].val_y);
  memmove(mem_u->row_u + i_z, mem_u->row_u + i_z + 1,
          (mem_u->len_z - i_z - 1) * sizeof(_row));
  mem_u->len_z--;
}

/* _undo_to(): unwind the journal back to [len_z] entries
*/
static void
_undo_to(_mem* mem_u, c3_z len_z)
{
  while ( mem_u->unl_z > len_z ) {
    _undo* und_u = &mem_u->und_u[--mem_u->unl_z];

    if ( c3y == und_u->had_o ) {
      _set(mem_u, und_u->row_u.key_y, und_u->row_u.key_z,
           und_u->row_u.val_y, und_u->row_u.val_z);
    }
    else {
      _unset(mem_u, und_u->row_u.key_y, und_u->row_u.key_z);
    }
    c3_free(und_u->row_u.key_y);
    c3_free(und_u->row_u.val_y);
  }
}

static int
_begin(void* ctx)
{
  _mem* mem_u = ctx;
  if ( c3y == mem_u->txn_o ) return -1;
  mem_u->txn_o = c3y;
  mem_u->man_w = 0;
  return 0;
}

static int
_mem_commit(void* ctx)
{
  _mem* mem_u = ctx;

  for ( c3_z i_z = 0; i_z < mem_u->unl_z; i_z++ ) {
    c3_free(mem_u->und_u[i_z].row_u.key_y);
    c3_free(mem_u->und_u[i_z].row_u.val_y);
  }
  mem_u->unl_z = 0;
  mem_u->man_w = 0;
  mem_u->txn_o = c3n;
  return 0;
}

static int
_rollback(void* ctx)
{
  _mem* mem_u = ctx;
  _undo_to(mem_u, 0);
  mem_u->man_w = 0;
  mem_u->txn_o = c3n;
  return 0;
}

static int
_mark(void* ctx)
{
  _mem* mem_u = ctx;
  if ( mem_u->man_w == 64 ) return -1;
  mem_u->mar_z[mem_u->man_w++] = mem_u->unl_z;
  return 0;
}

static int
_rewind(void* ctx)
{
  _mem* mem_u = ctx;
  if ( !mem_u->man_w ) return -1;
  _undo_to(mem_u, mem_u->mar_z[mem_u->man_w - 1]);
  return 0;
}

static int
_release(void* ctx)
{
  _mem* mem_u = ctx;
  if ( !mem_u->man_w ) return -1;
  mem_u->man_w--;
  return 0;
}

static int
_get(void* ctx, const uint8_t* key_y, size_t key_z,
     uint8_t** val_y, size_t* val_z)
{
  _mem* mem_u = ctx;
  c3_o  hit_o;
  c3_z  i_z = _find(mem_u, key_y, key_z, &hit_o);

  if ( c3n == hit_o ) {
    return 1;
  }
  *val_y = _mem_dup(mem_u->row_u[i_z].val_y, mem_u->row_u[i_z].val_z);
  *val_z = mem_u->row_u[i_z].val_z;
  return 0;
}

static int
_put(void* ctx, const uint8_t* key_y, size_t key_z,
     const uint8_t* val_y, size_t val_z)
{
  _mem* mem_u = ctx;
  _journal(mem_u, key_y, key_z);
  _set(mem_u, key_y, key_z, val_y, val_z);
  return 0;
}

static int
_del(void* ctx, const uint8_t* key_y, size_t key_z)
{
  _mem* mem_u = ctx;
  _journal(mem_u, key_y, key_z);
  _unset(mem_u, key_y, key_z);
  return 0;
}

static int
_scan(void* ctx, const uint8_t* pre_y, size_t pre_z,
      mesa_store_scan_f fun_f, void* arg)
{
  _mem* mem_u = ctx;
  c3_o  hit_o;
  c3_z  i_z = _find(mem_u, pre_y, pre_z, &hit_o);

  //  rows may be deleted by the callback; copy each one first
  //
  while ( i_z < mem_u->len_z ) {
    _row* row_u = &mem_u->row_u[i_z];
    c3_y* key_y;
    c3_y* val_y;
    c3_z  key_z, val_z;
    c3_i  ret_i;

    if (  (row_u->key_z < pre_z)
       || memcmp(row_u->key_y, pre_y, pre_z) )
    {
      break;
    }

    key_z = row_u->key_z;
    val_z = row_u->val_z;
    key_y = _mem_dup(row_u->key_y, key_z);
    val_y = _mem_dup(row_u->val_y, val_z);
    ret_i = fun_f(arg, key_y, key_z, val_y, val_z);

    //  resume after this key, wherever it is now
    //
    i_z = _find(mem_u, key_y, key_z, &hit_o);
    if ( c3y == hit_o ) i_z++;
    c3_free(key_y);
    c3_free(val_y);
    if ( ret_i ) return ret_i;
  }
  return 0;
}

static void
_mem_close(void* ctx)
{
  _mem* mem_u = ctx;

  _rollback(ctx);
  for ( c3_z i_z = 0; i_z < mem_u->len_z; i_z++ ) {
    c3_free(mem_u->row_u[i_z].key_y);
    c3_free(mem_u->row_u[i_z].val_y);
  }
  c3_free(mem_u->row_u);
  c3_free(mem_u->und_u);
  c3_free(mem_u);
}

mesa_store*
mesa_store_memory(void)
{
  _mem* mem_u = c3_calloc(sizeof(*mem_u));

  mem_u->txn_o = c3n;
  mem_u->sto_u = (mesa_store){
    .ctx      = mem_u,
    .begin    = _begin,
    .commit   = _mem_commit,
    .rollback = _rollback,
    .mark     = _mark,
    .rewind   = _rewind,
    .release  = _release,
    .get      = _get,
    .put      = _put,
    .del      = _del,
    .scan     = _scan,
    .close    = _mem_close,
  };
  return &mem_u->sto_u;
}

void
mesa_store_free(mesa_store* sto)
{
  if ( sto && sto->close ) {
    sto->close(sto->ctx);
  }
}

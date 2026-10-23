/// @file
///
/// The default mesa_store: one SQLite table, mesa_kv(k BLOB PRIMARY KEY,
/// v BLOB), in WAL mode with synchronous=FULL so a commit is durable
/// before libmesa releases any datagram that depends on it.
///
/// An exclusive flock on <path>.lock keeps a second process from running
/// the same state: two copies of a ship would reuse sequence numbers.

#include "compat.h"
#include "mesa/store.h"
#include "sqlite3.h"

#include <fcntl.h>
#include <unistd.h>
#ifdef _WIN32
#  include <io.h>
#  include <windows.h>
#else
#  include <sys/file.h>
#endif

typedef struct {
  mesa_store    sto_u;
  sqlite3*      sql_u;
  c3_i          lok_i;     //  lockfile descriptor
  c3_w          sav_w;     //  savepoint depth
  sqlite3_stmt* get_u;
  sqlite3_stmt* put_u;
  sqlite3_stmt* del_u;
} _sql;

/* _lock_take(): open [lok_c] and lock it exclusively, without waiting.
**   Returns the descriptor, -1 if it can't be opened, -2 if it's held.
*/
static c3_i
_lock_take(const c3_c* lok_c)
{
#ifdef _WIN32
  c3_i       fid_i = open(lok_c, O_RDWR | O_CREAT | O_BINARY | O_NOINHERIT, 0600);
  OVERLAPPED ove_u = {0};

  if ( 0 > fid_i ) {
    return -1;
  }
  if ( !LockFileEx((HANDLE)_get_osfhandle(fid_i),
                   LOCKFILE_EXCLUSIVE_LOCK | LOCKFILE_FAIL_IMMEDIATELY,
                   0, 1, 0, &ove_u) )
  {
    close(fid_i);
    return -2;
  }
  return fid_i;
#else
  c3_i fid_i = open(lok_c, O_RDWR | O_CREAT | O_CLOEXEC, 0600);

  if ( 0 > fid_i ) {
    return -1;
  }
  if ( flock(fid_i, LOCK_EX | LOCK_NB) ) {
    close(fid_i);
    return -2;
  }
  return fid_i;
#endif
}

/* _lock_drop(): unlock and close a descriptor from _lock_take().
*/
static void
_lock_drop(c3_i fid_i)
{
#ifdef _WIN32
  OVERLAPPED ove_u = {0};
  UnlockFileEx((HANDLE)_get_osfhandle(fid_i), 0, 1, 0, &ove_u);
#else
  flock(fid_i, LOCK_UN);
#endif
  close(fid_i);
}

static c3_i
_exec(_sql* sql_u, const c3_c* cmd_c)
{
  c3_c* err_c = 0;

  if ( SQLITE_OK != sqlite3_exec(sql_u->sql_u, cmd_c, 0, 0, &err_c) ) {
    mesa_log("mesa: sqlite: %s: %s", cmd_c, err_c ? err_c : "?");
    sqlite3_free(err_c);
    return -1;
  }
  return 0;
}

static int
_begin(void* ctx)
{
  _sql* sql_u = ctx;
  sql_u->sav_w = 0;
  return _exec(sql_u, "BEGIN IMMEDIATE");
}

static int
_sql_commit(void* ctx)
{
  return _exec(ctx, "COMMIT");
}

static int
_rollback(void* ctx)
{
  _sql* sql_u = ctx;

  sql_u->sav_w = 0;
  if ( sqlite3_get_autocommit(sql_u->sql_u) ) {
    return 0;   //  nothing open
  }
  return _exec(sql_u, "ROLLBACK");
}

static int
_mark(void* ctx)
{
  _sql* sql_u = ctx;
  c3_c  cmd_c[64];

  snprintf(cmd_c, sizeof(cmd_c), "SAVEPOINT mesa_%u", sql_u->sav_w);
  if ( _exec(sql_u, cmd_c) ) return -1;
  sql_u->sav_w++;
  return 0;
}

static int
_rewind(void* ctx)
{
  _sql* sql_u = ctx;
  c3_c  cmd_c[64];

  if ( !sql_u->sav_w ) return -1;
  snprintf(cmd_c, sizeof(cmd_c), "ROLLBACK TO mesa_%u", sql_u->sav_w - 1);
  return _exec(sql_u, cmd_c);
}

static int
_release(void* ctx)
{
  _sql* sql_u = ctx;
  c3_c  cmd_c[64];

  if ( !sql_u->sav_w ) return -1;
  snprintf(cmd_c, sizeof(cmd_c), "RELEASE mesa_%u", sql_u->sav_w - 1);
  if ( _exec(sql_u, cmd_c) ) return -1;
  sql_u->sav_w--;
  return 0;
}

static int
_get(void* ctx, const uint8_t* key_y, size_t key_z,
     uint8_t** val_y, size_t* val_z)
{
  _sql*         sql_u = ctx;
  sqlite3_stmt* stm_u = sql_u->get_u;
  c3_i          ret_i;

  sqlite3_reset(stm_u);
  sqlite3_bind_blob(stm_u, 1, key_y, (c3_i)key_z, SQLITE_STATIC);
  ret_i = sqlite3_step(stm_u);

  if ( SQLITE_DONE == ret_i ) {
    sqlite3_reset(stm_u);
    return 1;
  }
  if ( SQLITE_ROW != ret_i ) {
    mesa_log("mesa: sqlite: get: %s", sqlite3_errmsg(sql_u->sql_u));
    sqlite3_reset(stm_u);
    return -1;
  }

  *val_z = (size_t)sqlite3_column_bytes(stm_u, 0);
  *val_y = c3_malloc(c3_max(*val_z, 1));
  memcpy(*val_y, sqlite3_column_blob(stm_u, 0), *val_z);
  sqlite3_reset(stm_u);
  return 0;
}

static int
_put(void* ctx, const uint8_t* key_y, size_t key_z,
     const uint8_t* val_y, size_t val_z)
{
  _sql*         sql_u = ctx;
  sqlite3_stmt* stm_u = sql_u->put_u;
  c3_i          ret_i;

  sqlite3_reset(stm_u);
  sqlite3_bind_blob(stm_u, 1, key_y, (c3_i)key_z, SQLITE_STATIC);
  sqlite3_bind_blob(stm_u, 2, val_y, (c3_i)val_z, SQLITE_STATIC);
  ret_i = sqlite3_step(stm_u);
  sqlite3_reset(stm_u);

  if ( SQLITE_DONE != ret_i ) {
    mesa_log("mesa: sqlite: put: %s", sqlite3_errmsg(sql_u->sql_u));
    return -1;
  }
  return 0;
}

static int
_del(void* ctx, const uint8_t* key_y, size_t key_z)
{
  _sql*         sql_u = ctx;
  sqlite3_stmt* stm_u = sql_u->del_u;
  c3_i          ret_i;

  sqlite3_reset(stm_u);
  sqlite3_bind_blob(stm_u, 1, key_y, (c3_i)key_z, SQLITE_STATIC);
  ret_i = sqlite3_step(stm_u);
  sqlite3_reset(stm_u);

  if ( SQLITE_DONE != ret_i ) {
    mesa_log("mesa: sqlite: del: %s", sqlite3_errmsg(sql_u->sql_u));
    return -1;
  }
  return 0;
}

typedef struct {
  c3_y* key_y;
  c3_z  key_z;
  c3_y* val_y;
  c3_z  val_z;
} _row;

/* _scan(): rows are read into memory first, so callbacks may write
*/
static int
_scan(void* ctx, const uint8_t* pre_y, size_t pre_z,
      mesa_store_scan_f fun_f, void* arg)
{
  _sql*         sql_u = ctx;
  sqlite3_stmt* stm_u;
  c3_y*         end_y = 0;
  c3_z          end_z = pre_z;
  _row*         row_u = 0;
  c3_z          len_z = 0, cap_z = 0;
  c3_i          ret_i = 0;

  //  upper bound: the prefix plus one, if it has one
  //
  if ( pre_z ) {
    end_y = c3_malloc(pre_z);
    memcpy(end_y, pre_y, pre_z);
    while ( end_z && (0xff == end_y[end_z - 1]) ) end_z--;
    if ( end_z ) end_y[end_z - 1]++;
  }

  if ( end_z ) {
    ret_i = sqlite3_prepare_v2(sql_u->sql_u,
      "SELECT k, v FROM mesa_kv WHERE k >= ?1 AND k < ?2 ORDER BY k",
      -1, &stm_u, 0);
    if ( SQLITE_OK == ret_i ) {
      sqlite3_bind_blob(stm_u, 2, end_y, (c3_i)end_z, SQLITE_STATIC);
    }
  }
  else {
    ret_i = sqlite3_prepare_v2(sql_u->sql_u,
      "SELECT k, v FROM mesa_kv WHERE k >= ?1 ORDER BY k", -1, &stm_u, 0);
  }
  if ( SQLITE_OK != ret_i ) {
    c3_free(end_y);
    return -1;
  }
  sqlite3_bind_blob(stm_u, 1, pre_y, (c3_i)pre_z, SQLITE_STATIC);

  while ( SQLITE_ROW == (ret_i = sqlite3_step(stm_u)) ) {
    _row* r;
    if ( len_z == cap_z ) {
      cap_z = c3_max(64, cap_z * 2);
      row_u = c3_realloc(row_u, cap_z * sizeof(_row));
    }
    r = &row_u[len_z++];
    r->key_z = (c3_z)sqlite3_column_bytes(stm_u, 0);
    r->key_y = c3_malloc(c3_max(r->key_z, 1));
    memcpy(r->key_y, sqlite3_column_blob(stm_u, 0), r->key_z);
    r->val_z = (c3_z)sqlite3_column_bytes(stm_u, 1);
    r->val_y = c3_malloc(c3_max(r->val_z, 1));
    memcpy(r->val_y, sqlite3_column_blob(stm_u, 1), r->val_z);
  }
  sqlite3_finalize(stm_u);
  c3_free(end_y);

  //  a failed read: no partial results
  //
  if ( SQLITE_DONE != ret_i ) {
    for ( c3_z i = 0; i < len_z; i++ ) {
      c3_free(row_u[i].key_y);
      c3_free(row_u[i].val_y);
    }
    c3_free(row_u);
    return -1;
  }
  ret_i = 0;

  for ( c3_z i = 0; i < len_z; i++ ) {
    if ( !ret_i ) {
      ret_i = fun_f(arg, row_u[i].key_y, row_u[i].key_z,
                    row_u[i].val_y, row_u[i].val_z);
    }
    c3_free(row_u[i].key_y);
    c3_free(row_u[i].val_y);
  }
  c3_free(row_u);
  return ret_i;
}

static void
_sql_close(void* ctx)
{
  _sql* sql_u = ctx;

  sqlite3_finalize(sql_u->get_u);
  sqlite3_finalize(sql_u->put_u);
  sqlite3_finalize(sql_u->del_u);
  sqlite3_close(sql_u->sql_u);
  if ( 0 <= sql_u->lok_i ) {
    _lock_drop(sql_u->lok_i);
  }
  c3_free(sql_u);
}

mesa_store*
mesa_store_sqlite(const char* path, const char** err)
{
  _sql* sql_u = c3_calloc(sizeof(*sql_u));
  c3_c* lok_c;

  *err = 0;
  sql_u->lok_i = -1;

  //  one process per state
  //
  lok_c = c3_malloc(strlen(path) + 8);
  sprintf(lok_c, "%s.lock", path);
  sql_u->lok_i = _lock_take(lok_c);
  c3_free(lok_c);

  if ( 0 > sql_u->lok_i ) {
    *err = ( -2 == sql_u->lok_i ) ? "state is in use by another process"
                                  : "can't open lockfile";
    c3_free(sql_u);
    return 0;
  }

  if ( SQLITE_OK != sqlite3_open_v2(path, &sql_u->sql_u,
                                    SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE,
                                    0) )
  {
    *err = "can't open database";
    sqlite3_close(sql_u->sql_u);
    sql_u->sql_u = 0;
    goto fail;
  }

  if (  _exec(sql_u, "PRAGMA journal_mode=WAL")
     || _exec(sql_u, "PRAGMA synchronous=FULL")
     || _exec(sql_u, "CREATE TABLE IF NOT EXISTS mesa_kv "
                     "(k BLOB PRIMARY KEY, v BLOB NOT NULL) WITHOUT ROWID") )
  {
    *err = "can't initialize database";
    goto fail;
  }

  if (  sqlite3_prepare_v2(sql_u->sql_u,
          "SELECT v FROM mesa_kv WHERE k = ?1", -1, &sql_u->get_u, 0)
     || sqlite3_prepare_v2(sql_u->sql_u,
          "INSERT OR REPLACE INTO mesa_kv (k, v) VALUES (?1, ?2)",
          -1, &sql_u->put_u, 0)
     || sqlite3_prepare_v2(sql_u->sql_u,
          "DELETE FROM mesa_kv WHERE k = ?1", -1, &sql_u->del_u, 0) )
  {
    *err = "can't prepare statements";
    goto fail;
  }

  sql_u->sto_u = (mesa_store){
    .ctx      = sql_u,
    .begin    = _begin,
    .commit   = _sql_commit,
    .rollback = _rollback,
    .mark     = _mark,
    .rewind   = _rewind,
    .release  = _release,
    .get      = _get,
    .put      = _put,
    .del      = _del,
    .scan     = _scan,
    .close    = _sql_close,
  };
  return &sql_u->sto_u;

fail:
  _sql_close(sql_u);
  return 0;
}

void*
mesa_store_sqlite_db(mesa_store* sto)
{
  return ((_sql*)sto->ctx)->sql_u;
}

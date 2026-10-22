# The store, and writing your own

libmesa keeps all protocol state (identity, peers, flows, queued
messages) in a `mesa_store`: a transactional key-value store with
savepoints, declared in `include/mesa/store.h`. The engine writes to it
inside the same transaction as the application's own changes. That is
where exactly-once delivery comes from (see the
[README](../README.md#exactly-once-delivery)).

Two stores come with the library: `mesa_store_sqlite()` (persistent, the
default) and `mesa_store_memory()` (volatile). This page covers what the
engine expects of a store, what it puts there, how the two built-in
stores behave, and how to write another one.

See also [getting-started.md](getting-started.md), [host.md](host.md),
[urbit-apps.md](urbit-apps.md) and [python.md](python.md).

## The contract

```c
typedef struct mesa_store {
  void* ctx;
  int (*begin)(void* ctx);
  int (*commit)(void* ctx);
  int (*rollback)(void* ctx);
  int (*mark)(void* ctx);       //  savepoint            (optional)
  int (*rewind)(void* ctx);     //  roll back to it      (optional)
  int (*release)(void* ctx);    //  forget it            (optional)
  int (*get)(void* ctx, const uint8_t* key, size_t key_len,
             uint8_t** val, size_t* val_len);
  int (*put)(void* ctx, const uint8_t* key, size_t key_len,
             const uint8_t* val, size_t val_len);
  int (*del)(void* ctx, const uint8_t* key, size_t key_len);
  int (*scan)(void* ctx, const uint8_t* pre, size_t pre_len,
              mesa_store_scan_f fun, void* arg);
  void (*close)(void* ctx);
} mesa_store;
```

Every function gets `ctx` as its first argument. Keys and values are
byte strings. All functions but `get` and `scan` return 0 on success and
nonzero on failure.

| function | returns | the engine on failure |
| --- | --- | --- |
| `begin` | 0 | stops (`MESA_E_FAILED`) |
| `commit` | 0 | stops; the event's datagrams are dropped |
| `rollback` | ignored | -- |
| `mark` | 0 | runs that handler without a savepoint (below) |
| `rewind`, `release` | ignored | -- |
| `get` | 0 found, 1 missing, -1 error | `MESA_E_STORE` from `mesa_engine_open` |
| `put`, `del` | 0 | stops |
| `scan` | 0 done, the callback's positive value if it stopped, negative on error | `MESA_E_STORE` from `mesa_engine_open`; stops on a reload |

### Transactions

- `begin` opens a transaction, `commit` makes it durable, `rollback`
  undoes it. The engine calls `rollback` when an event fails after a
  successful `begin`, on `mesa_txn_abort`, and on `mesa_engine_close`
  with a transaction open; its return value is ignored. Making it a
  no-op when nothing is open (as the SQLite store does) is prudent.
- The store never sees nested transactions. The engine counts nesting
  itself (`eng_txn_open`/`eng_txn_close` in `src/eng/eng_util.c`):
  engine calls made inside an application transaction
  (`mesa_txn_begin`) or inside a hook join the open one, and only the
  outermost close calls `commit`. There is one `begin` and one
  `commit` or `rollback` per event.
- Commit must be atomic and durable when it returns. The engine hands
  the event's datagrams to `hooks.send` only after `commit` returns 0,
  and an ack sent for a commit that is later lost breaks exactly-once.
- Read-your-writes: `get` and `scan` inside a transaction see its own
  `put`s and `del`s. The application's reads in the same transaction
  (its own tables, with SQLite) need the same.
- `get` and `scan` are also called outside any transaction: at
  `mesa_engine_open` (the identity check and loading state), and when
  the engine rebuilds its memory after a rollback (`eng_reload`). All
  engine writes happen inside a transaction.

### get

On a hit, `get` returns 0 and a buffer the engine frees. The engine frees
it with libmesa's allocator, so it must come from `mesa_malloc()`
(`include/mesa/noun.h`). In C linked against the same libc as libmesa,
`mesa_malloc` is `malloc`; an FFI store whose language has its own
allocator must call `mesa_malloc` (the Python store does). A
zero-length value still needs a non-null buffer the engine can free;
`mesa_malloc(0)` gives one.

### scan

`scan` calls `fun` for every row whose key starts with `pre`, in
byte-wise key order (`memcmp` on the common length, then shorter
first). `fun` returns 0 to go on, or a positive number to stop, which
`scan` then returns; `scan` returns 0 when it has gone through every row,
and a negative number if the store failed. An empty prefix means every
row. The key and value pointers are only good
for the call.

- `fun` may `put` and `del` during the scan: when it moves to a new
  rift, the engine deletes rows from inside a full scan (`_del_row` in
  `src/eng/engine.c`). Both built-in stores copy rows before calling
  back for this reason.
- Order matters: queued messages are loaded in key order, which is
  sequence order.
- The engine scans when it opens and when it reloads its memory after
  a rollback. A negative return fails `mesa_engine_open` with
  `MESA_E_STORE`, or stops the engine (`MESA_E_FAILED`) on a reload. If
  you can, fail before
  delivering any row: read the rows up front, as the SQLite store does
  (it returns -1 on a prepare or step error, having called back for
  nothing).

### close

`close` frees the store and everything it holds. `mesa_store_free(sto)`
calls `sto->close(sto->ctx)` and works for any store. The engine never
closes the store: `mesa_engine_close` rolls back an open transaction
and leaves the store to its owner.

## Savepoints

`mark`, `rewind` and `release` are a stack of savepoints inside the open
transaction: `mark` pushes one, `rewind` undoes everything since the top
one and keeps it, `release` pops it and keeps its writes. A store
without them sets all three to NULL. The engine checks only `mark`: if
`mark` is set, `rewind` and `release` must be too.

The engine uses one savepoint at a time, around each call of
`hooks.plea` (`src/eng/eng_flow.c`), so a depth of one is enough. Hooks
can't open transactions of their own (`mesa_txn_begin` returns
`MESA_E_STATE` there), so there is no deeper nesting from the
application either.

What the handler's return value does:

| result | with savepoints | without |
| --- | --- | --- |
| `MESA_ACK`, `MESA_LATER` | `release`; the writes stay | the writes stay |
| `MESA_NACK` | `rewind`, `release`; the handler's datagrams are cut; the nack and error are recorded | the engine stops (`MESA_E_FAILED`); the whole event is rolled back |
| `MESA_DROP` | `rewind`, `release`; the handler's datagrams are cut; no answer | no answer, nothing undone: the handler must not have written |

If a rewound handler called the engine's API (sent a message, opened a
flow), the engine's memory no longer matches the store; it rebuilds it
from the store after the event commits.

Without savepoints, then, a nack is not just the event failing: the
engine stops, and when the host reopens it the peer resends, the
handler nacks again, and the engine stops again. A store without
savepoints suits applications that never nack and never write before
dropping. If `mark` fails at runtime (the memory store's stack is 64
deep), that handler runs as if the store had no savepoints.

## Failure

Any failed `begin`, `commit`, `put` or `del` (or `scan` on a reload)
stops the engine (`eng_txn_fail`): it logs `mesa: store failed; engine stopped`, rolls
back the event at its end, drops its datagrams, and from then on refuses
all work. API calls return `MESA_E_FAILED`, and `mesa_engine_hear` and
`mesa_engine_wake` do nothing. Its memory may no longer match the
store, so there is no recovery in place: close the engine
(`mesa_engine_close`) and open a new one on the same store, which
resumes from the last commit. Peers resend whatever wasn't acked.

After a failed `commit` the engine calls `rollback`, so the store isn't
left in the middle of a transaction for the engine that reopens it.

libmesa-host doesn't do this for you; see
[host.md](host.md#engine-failure).

## What's stored

Keys are built by `eng_key()` in `src/eng/eng_util.c`. The first byte is
the row type; integers are big-endian so that scans come back in order.

| type | byte | layout | length | row |
| --- | --- | --- | --- | --- |
| `KEY_IDENT` | `'I'` | type | 1 | our identity |
| `KEY_PEER` | `'P'` | type, ship | 17 | a peer: keys, sponsor, next bone, system flows |
| `KEY_FLOW` | `'F'` | type, ship, bone, dire | 26 | a flow's state |
| `KEY_CORK` | `'C'` | type, ship, bone, dire | 26 | a corked (closed) flow |
| `KEY_LOAD` | `'L'` | type, ship, bone, dire, seq | 34 | a message we sent on a flow, not yet acked |

- ship: 16 bytes, the 128-bit `@p` big-endian;
- bone: 8 bytes;
- dire: 1 byte, 0 for a flow we send pleas on, 1 for one we send boons
  on;
- seq: 8 bytes, the message's sequence number on its flow.

Values are jammed nouns. The identity row is
`[[ship rift life ring] ping-bone=(unit @)]`; treat the rest as opaque.
Neither the key layout nor the values are a stable public API: they can
change between versions, and only libmesa should read or write them.

Keep application data out of this key space. On open, a store that has
rows but no identity row is refused (`MESA_E_IDENTITY`), and when the
identity's rift has gone up (a breach, opened with `breached`), the
engine deletes every row except the identity row. With SQLite, use your
own tables instead (below).

## The SQLite store

```c
const char* err;
mesa_store* sto = mesa_store_sqlite("ship.db", &err);   //  NULL, err set
...
mesa_store_free(sto);
```

`src/store_sqlite.c`:

- One table, `mesa_kv (k BLOB PRIMARY KEY, v BLOB NOT NULL) WITHOUT
  ROWID`, created if missing. SQLite compares blobs with `memcmp`, then
  length, which is the order `scan` needs.
- `PRAGMA journal_mode=WAL` and `synchronous=FULL`, so a commit is on
  disk before any datagram that depends on it leaves.
- `begin` is `BEGIN IMMEDIATE`; savepoints are SQL savepoints
  (`SAVEPOINT mesa_<n>`, `ROLLBACK TO`, `RELEASE`), any depth.
- `scan` reads the matching rows into memory before calling back, so
  the callback can write.
- An exclusive `flock` on `<path>.lock` (non-blocking), held until the
  store is closed. A second open of the same state, from another
  process or the same one, fails with `"state is in use by another
  process"`: two copies of a ship would reuse sequence numbers. The
  `.lock` file stays behind after close; that's harmless.

Other errors are `"can't open lockfile"`, `"can't open database"`,
`"can't initialize database"` and `"can't prepare statements"`. SQL
errors at runtime are logged through `mesa_log`.

### Your tables in the same transaction

`mesa_store_sqlite_db(sto)` returns the store's `sqlite3*`. Create your
tables in that database and write them with that handle, and your
writes join whatever transaction the engine has open:

- inside `hooks.plea`, `hooks.boon` and `hooks.done`, they commit with
  the event, and the ack or nack goes out only after that commit;
- a plea handler's writes are inside its savepoint, so `MESA_NACK` and
  `MESA_DROP` undo them;
- elsewhere, wrap your writes and the engine calls that go with them in
  `mesa_txn_begin`/`mesa_txn_commit`; `mesa_txn_abort` rolls both back.

`examples/guestbook.c` does this. At startup it takes the handle and
creates `gb_entry`, `gb_sub` and `gb_outbox` in it. Its plea handler
inserts the entry, sends facts to subscribers (`mesa_gall_fact`) and
appends to the outbox, then returns `MESA_ACK`: the row, the facts and
the ack commit together or not at all. The outbox is drained after each
commit, by POSTing the oldest row to a webhook and deleting it inside
`mesa_txn_begin`/`mesa_txn_commit` once the POST succeeds. The webhook
sees each entry at least once (a crash between the POST and the delete
repeats it), with an id to deduplicate on.

The Python binding's `SqliteStore` is the same schema, so a database is
usable from either.

## The memory store

`mesa_store_memory()` (`src/store_mem.c`) is a sorted array of rows
with an undo journal: each `put` and `del` in a transaction records the
key's old value, `rollback` replays the journal backwards, and a
savepoint is a position in the journal (a stack of 64). `begin` fails if
a transaction is already open. Writes outside a transaction apply
directly. `get` never fails. `scan` copies each row before calling back
and finds its place again afterwards, so the callback can write.

Everything is lost when the process exits, and a fresh memory store is
an empty store: `mesa_engine_open` refuses it (`MESA_E_FRESH`) unless the
config says `breached` or the ship is a comet. Use it for tests, and for
identities that can start fresh every run.

## Writing your own

A store needs an ordered key space (for `scan`), atomic durable commits,
read-your-writes, and, ideally, savepoints. Here is one over LMDB, whose
nested transactions give the savepoints. LMDB's default key order is the
byte-wise order `scan` needs.

```c
#include <lmdb.h>
#include <stdlib.h>
#include <string.h>

#include "mesa/noun.h"    //  mesa_malloc
#include "mesa/store.h"

typedef struct {
  mesa_store sto;
  MDB_env*   env;
  MDB_dbi    dbi;
  MDB_txn*   txn[8];      //  [0] the transaction, then one per savepoint
  int        dep;         //  how many are open
} lm;

static MDB_txn* _top(lm* l) { return l->dep ? l->txn[l->dep - 1] : 0; }

static int
_begin(void* ctx)
{
  lm* l = ctx;
  if ( l->dep || mdb_txn_begin(l->env, 0, 0, &l->txn[0]) ) return -1;
  l->dep = 1;
  return 0;
}

static int
_commit(void* ctx)
{
  lm* l = ctx;
  int r = 0;
  if ( !l->dep ) return -1;
  while ( l->dep ) r |= mdb_txn_commit(l->txn[--l->dep]);  //  children first
  return r ? -1 : 0;
}

static int
_rollback(void* ctx)
{
  lm* l = ctx;
  while ( l->dep ) mdb_txn_abort(l->txn[--l->dep]);
  return 0;
}

static int
_mark(void* ctx)
{
  lm* l = ctx;
  if ( !l->dep || (8 == l->dep) ) return -1;
  if ( mdb_txn_begin(l->env, _top(l), 0, &l->txn[l->dep]) ) return -1;
  l->dep++;
  return 0;
}

static int
_rewind(void* ctx)        //  undo the savepoint's writes; keep it open
{
  lm* l = ctx;
  if ( l->dep < 2 ) return -1;
  mdb_txn_abort(l->txn[l->dep - 1]);
  if ( mdb_txn_begin(l->env, l->txn[l->dep - 2], 0, &l->txn[l->dep - 1]) ) {
    l->dep--;
    return -1;
  }
  return 0;
}

static int
_release(void* ctx)       //  keep its writes in the parent
{
  lm* l = ctx;
  if ( l->dep < 2 ) return -1;
  return mdb_txn_commit(l->txn[--l->dep]) ? -1 : 0;
}

static int
_get(void* ctx, const uint8_t* key, size_t key_len,
     uint8_t** val, size_t* val_len)
{
  lm*      l   = ctx;
  MDB_txn* txn = _top(l);
  MDB_val  k   = { key_len, (void*)key }, v;
  int      r;

  //  outside a transaction (at open): a read-only one
  if ( !txn && mdb_txn_begin(l->env, 0, MDB_RDONLY, &txn) ) return -1;
  r = mdb_get(txn, l->dbi, &k, &v);
  if ( 0 == r ) {
    *val = mesa_malloc(v.mv_size);          //  the engine frees it
    memcpy(*val, v.mv_data, v.mv_size);
    *val_len = v.mv_size;
  }
  if ( !l->dep ) mdb_txn_abort(txn);
  return ( 0 == r ) ? 0 : ( MDB_NOTFOUND == r ) ? 1 : -1;
}

static int
_put(void* ctx, const uint8_t* key, size_t key_len,
     const uint8_t* val, size_t val_len)
{
  lm*     l = ctx;
  MDB_val k = { key_len, (void*)key }, v = { val_len, (void*)val };
  return ( l->dep && !mdb_put(_top(l), l->dbi, &k, &v, 0) ) ? 0 : -1;
}

static int
_del(void* ctx, const uint8_t* key, size_t key_len)
{
  lm*     l = ctx;
  MDB_val k = { key_len, (void*)key };
  int     r = l->dep ? mdb_del(_top(l), l->dbi, &k, 0) : -1;
  return ( 0 == r || MDB_NOTFOUND == r ) ? 0 : -1;
}

typedef struct { MDB_val k, v; } row;

static int
_scan(void* ctx, const uint8_t* pre, size_t pre_len,
      mesa_store_scan_f fun, void* arg)
{
  lm*         l   = ctx;
  MDB_txn*    txn = _top(l);
  MDB_cursor* cur;
  MDB_val     k   = { pre_len, (void*)pre }, v;
  row*        rows = 0;
  size_t      n = 0, cap = 0;
  int         op = pre_len ? MDB_SET_RANGE : MDB_FIRST;   //  no empty keys
  int         ret = 0;

  if ( !txn && mdb_txn_begin(l->env, 0, MDB_RDONLY, &txn) ) return -1;
  if ( mdb_cursor_open(txn, l->dbi, &cur) ) {
    if ( !l->dep ) mdb_txn_abort(txn);
    return -1;
  }

  //  read the rows first: the callback may write
  while ( 0 == mdb_cursor_get(cur, &k, &v, op) ) {
    op = MDB_NEXT;
    if ( k.mv_size < pre_len || memcmp(k.mv_data, pre, pre_len) ) break;
    if ( n == cap ) rows = realloc(rows, (cap = cap ? 2 * cap : 64) * sizeof(row));
    rows[n].k.mv_size = k.mv_size;
    rows[n].k.mv_data = memcpy(malloc(k.mv_size + 1), k.mv_data, k.mv_size);
    rows[n].v.mv_size = v.mv_size;
    rows[n].v.mv_data = memcpy(malloc(v.mv_size + 1), v.mv_data, v.mv_size);
    n++;
  }
  mdb_cursor_close(cur);
  if ( !l->dep ) mdb_txn_abort(txn);

  for ( size_t i = 0; i < n; i++ ) {
    if ( !ret ) ret = fun(arg, rows[i].k.mv_data, rows[i].k.mv_size,
                               rows[i].v.mv_data, rows[i].v.mv_size);
    free(rows[i].k.mv_data);
    free(rows[i].v.mv_data);
  }
  free(rows);
  return ret;
}

static void
_close(void* ctx)
{
  lm* l = ctx;
  _rollback(l);
  mdb_env_close(l->env);
  free(l);
}

mesa_store*
store_lmdb(const char* dir)     //  NULL on failure
{
  lm*      l = calloc(1, sizeof(*l));
  MDB_txn* txn;

  if (  mdb_env_create(&l->env)
     || mdb_env_set_mapsize(l->env, (size_t)1 << 32)
     || mdb_env_open(l->env, dir, 0, 0600)
     || mdb_txn_begin(l->env, 0, 0, &txn) )
  {
    mdb_env_close(l->env);
    free(l);
    return 0;
  }
  if ( mdb_dbi_open(txn, 0, 0, &l->dbi) || mdb_txn_commit(txn) ) {
    mdb_env_close(l->env);
    free(l);
    return 0;
  }

  l->sto = (mesa_store){
    .ctx = l, .begin = _begin, .commit = _commit, .rollback = _rollback,
    .mark = _mark, .rewind = _rewind, .release = _release,
    .get = _get, .put = _put, .del = _del, .scan = _scan, .close = _close,
  };
  return &l->sto;
}
```

Notes on it:

- LMDB already locks the environment against concurrent writers, but
  not against a second process running the same ship between
  transactions. Take an exclusive lock for the store's lifetime, as the
  SQLite store does with `flock` on `<path>.lock`.
- An application keeping its own state in LMDB would open its own named
  database in the same environment and write it in `_top()`, the same
  transaction the engine is using. A store that can't share its
  transaction with the application still gives correct protocol state,
  but not exactly-once processing of the application's changes.
- `scan` returns -1 before calling back if it can't read; see
  [scan](#scan). (A cursor error midway just ends the read early; check
  `mdb_cursor_get`'s result for `MDB_NOTFOUND` to be strict.)

To check a store, `test/store_test.c` exercises both built-in stores'
key-value semantics (prefix scans including `0xff` bytes, savepoints)
and exactly-once delivery across crash-restarts; it's a model for
testing your own.

### FFI stores

The struct is plain function pointers and a `ctx`, so a store can be
written in any language with C callbacks. `SqliteStore` in
`bindings/python/libmesa.py` is one, in Python with ctypes, over the
standard `sqlite3` module (`isolation_level=None`, so the SQL is the
store's own):

- it creates the same `mesa_kv` table, with WAL and `synchronous=FULL`,
  and `flock`s `<path>.lock`;
- `begin` is `BEGIN IMMEDIATE`; savepoints are `SAVEPOINT m<n>` by
  depth;
- `get` copies the value into `lib.mesa_malloc(len)`, since the engine
  frees it with `mesa_free`;
- `scan` fetches all matching rows before calling back;
- every callback is wrapped so that a Python exception is printed and
  returned as -1, never thrown across the C boundary;
- the application's tables live in the same connection (`node.db`), so
  handler writes commit with the ack.

For any FFI store: keep the callback objects alive as long as the
engine can call them (ctypes frees a callback whose Python object is
collected; the binding keeps them in `self.c`), allocate `get`'s value
with `mesa_malloc`, and never let an exception or panic unwind through
libmesa. See [python.md](python.md).

## Why it's exactly once

Each input to the engine (a datagram, a timer, a point, an API call) is
one store transaction. When a plea arrives:

1. the engine records it on the flow, calls `hooks.plea`, which writes
   the application's tables in the same transaction, and records the
   ack (or the nack and its error);
2. the transaction commits;
3. only then is the ack datagram handed to `hooks.send`.

A crash before step 2 loses both the application's change and the ack:
the store holds neither. The peer never got an ack, so it resends, and
the restarted engine handles the message as new. A crash after step 2
keeps both: the flow's state says the message was handled, so when the
peer resends, the engine answers with the recorded ack without calling
the handler again. Either way the handler's effect is committed exactly once.

Outbound messages work the same way: `mesa_flow_plea` and friends write
the message as a `KEY_LOAD` row in the open transaction. Commit it with
the application state that produced it and the message is sent (and
resent until acked, across restarts) if and only if that state exists.

This covers what lives in the store's transaction. Effects outside it
(HTTP, files, other databases) can't be rolled back, so they can repeat
after a crash. Record them in a table in the same transaction (an
outbox) and perform them after the commit, deduplicating at the
receiver, as `examples/guestbook.c` does with its webhook.

`test/guestbook/crash.sh` tests the whole chain. It runs
`mesa-guestbook` as fake `~nec` with a webhook (`test/guestbook/hook.py`)
that fails 30% of the time, signs it 40 times from `~bud` with
`mesa-ping`, and meanwhile kills the guestbook with `kill -9` every 1-3
seconds (25 times) and restarts it. It passes when all 40 signatures
were acked, `gb_entry` holds exactly 40 rows with 40 distinct texts,
and the webhook saw 40 distinct ids. It needs `zig build`, python3, UDP
ports 31338-31339 and TCP port 18080.

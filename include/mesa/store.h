/// @file
///
/// Pluggable persistence: a transactional key-value store.
///
/// libmesa keeps all protocol state (identity, peers, flows, queued
/// messages) in a store and writes it inside the same transaction as the
/// application's own changes, which is what makes delivery exactly-once:
/// a message is acked only after the transaction that handled it commits.
///
/// Keys and values are opaque bytes. Implementations must provide
/// read-your-writes inside a transaction and atomic commit that is
/// durable when commit() returns (datagrams go out right after).
/// libmesa never nests begin(); it holds at most one savepoint.
///
/// Savepoints (mark/rewind/release) let libmesa undo an application
/// handler's writes when it nacks or drops a message. A store may leave
/// all three NULL; then a nack stops the engine (MESA_E_FAILED), and a
/// handler that drops must not have written. Provide them unless your
/// handlers only ever ack. With mark set, rewind and release must work.

#ifndef MESA_STORE_H
#define MESA_STORE_H

#include <stddef.h>
#include <stdint.h>

/* mesa_store_scan_f: one row of a prefix scan; return a positive number
**                    to stop (scan() then returns it), 0 to go on. It may
**                    write to the store (put or del any row).
*/
typedef int (*mesa_store_scan_f)(void* arg,
                                 const uint8_t* key, size_t key_len,
                                 const uint8_t* val, size_t val_len);

typedef struct mesa_store {
  void* ctx;

  //  all return 0 on success
  //
  int (*begin)(void* ctx);
  int (*commit)(void* ctx);
  int (*rollback)(void* ctx);

  int (*mark)(void* ctx);       //  savepoint            (optional)
  int (*rewind)(void* ctx);     //  roll back to it      (optional)
  int (*release)(void* ctx);    //  forget it            (optional)

  //  get: 0 and a malloc'd value, 1 if missing, -1 on error
  //
  int (*get)(void* ctx, const uint8_t* key, size_t key_len,
             uint8_t** val, size_t* val_len);
  int (*put)(void* ctx, const uint8_t* key, size_t key_len,
             const uint8_t* val, size_t val_len);
  int (*del)(void* ctx, const uint8_t* key, size_t key_len);

  //  scan: every row whose key starts with [pre], in byte-wise key
  //        order (an empty prefix: every row). 0 when done, the
  //        callback's value if it stopped, negative on error (with no
  //        rows delivered, if it can manage that).
  //
  //  get and scan are also called outside any transaction (when the
  //  engine opens or reloads).
  //
  int (*scan)(void* ctx, const uint8_t* pre, size_t pre_len,
              mesa_store_scan_f fun, void* arg);

  void (*close)(void* ctx);
} mesa_store;

/* mesa_store_memory(): a volatile in-memory store (tests, ephemeral use)
*/
mesa_store*
mesa_store_memory(void);

/* mesa_store_sqlite(): the default persistent store.
**
**   Opens (creating if needed) the database at [path] and takes an
**   exclusive lock on [path].lock, so two processes can never run the
**   same state. Returns NULL and sets *err (static string) on failure.
*/
mesa_store*
mesa_store_sqlite(const char* path, const char** err);

/* mesa_store_sqlite_db(): the store's sqlite3* handle, for applications
**                         that keep their own tables in the same
**                         database to get exactly-once processing.
*/
void*
mesa_store_sqlite_db(mesa_store* sto);

/* mesa_store_free(): close and free a store
*/
void
mesa_store_free(mesa_store* sto);

#endif /* ifndef MESA_STORE_H */

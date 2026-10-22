package mesa

// #include "shim.h"
import "C"

import (
	"bytes"
	"context"
	"database/sql"
	"errors"
	"fmt"
	"os"
	"runtime/cgo"
	"sort"
	"syscall"
	"unsafe"
)

// Store keeps libmesa's state: a transactional key-value store (see
// docs/store.md). The engine calls it from its own goroutine only.
//
// Begin, Commit and Rollback bracket each event; Get and Scan may also be
// called outside one. A Scan callback may Put and Delete, so Scan must
// not iterate over live state. Commit must be durable when it returns.
type Store interface {
	Begin() error
	Commit() error
	Rollback() error
	Get(key []byte) (val []byte, ok bool, err error)
	Put(key, val []byte) error
	Delete(key []byte) error
	// Scan calls fn for every row whose key starts with prefix, in
	// byte-wise key order, until fn returns true.
	Scan(prefix []byte, fn func(key, val []byte) (stop bool)) error
}

// Savepoints lets the engine undo a handler's writes when it nacks or
// drops a message. A Store without them can't be used by an
// application that nacks (the engine stops instead).
type Savepoints interface {
	Mark() error
	Rewind() error
	Release() error
}

// cstore is a Store made callable from C.
type cstore struct {
	s   Store
	sp  Savepoints
	c   *C.mesa_store
	h   cgo.Handle
	err error // the last failure, for the log
}

func newCStore(s Store) *cstore {
	cs := &cstore{s: s}
	cs.sp, _ = s.(Savepoints)
	cs.h = cgo.NewHandle(cs)
	sav := C.int(0)
	if cs.sp != nil {
		sav = 1
	}
	cs.c = C.mesa_go_store(C.uintptr_t(cs.h), sav)
	return cs
}

func (cs *cstore) free() {
	C.free(unsafe.Pointer(cs.c))
	cs.h.Delete()
}

func (cs *cstore) ret(err error) C.int {
	if err != nil {
		cs.err = err
		logf("mesa: store: %v", err)
		return -1
	}
	return 0
}

func storeOf(ctx C.uintptr_t) *cstore { return cgo.Handle(ctx).Value().(*cstore) }

//export goStoreOp
func goStoreOp(ctx C.uintptr_t, op C.int) (r C.int) {
	cs := storeOf(ctx)
	defer func() {
		if p := recover(); p != nil {
			r = cs.ret(fmt.Errorf("panic: %v", p))
		}
	}()
	switch op {
	case 0:
		return cs.ret(cs.s.Begin())
	case 1:
		return cs.ret(cs.s.Commit())
	case 2:
		return cs.ret(cs.s.Rollback())
	case 3:
		return cs.ret(cs.sp.Mark())
	case 4:
		return cs.ret(cs.sp.Rewind())
	case 5:
		return cs.ret(cs.sp.Release())
	}
	return -1
}

//export goStoreGet
func goStoreGet(ctx C.uintptr_t, key *C.uint8_t, keyLen C.size_t, val **C.uint8_t, valLen *C.size_t) (r C.int) {
	cs := storeOf(ctx)
	defer func() {
		if p := recover(); p != nil {
			r = cs.ret(fmt.Errorf("panic: %v", p))
		}
	}()
	v, ok, err := cs.s.Get(C.GoBytes(unsafe.Pointer(key), C.int(keyLen)))
	if err != nil {
		return cs.ret(err)
	}
	if !ok {
		return 1
	}
	//  the engine frees it with mesa_free
	p := C.mesa_malloc(C.size_t(len(v)))
	if len(v) > 0 {
		C.memcpy(p, unsafe.Pointer(&v[0]), C.size_t(len(v)))
	}
	*val = (*C.uint8_t)(p)
	*valLen = C.size_t(len(v))
	return 0
}

//export goStorePut
func goStorePut(ctx C.uintptr_t, key *C.uint8_t, keyLen C.size_t, val *C.uint8_t, valLen C.size_t) (r C.int) {
	cs := storeOf(ctx)
	defer func() {
		if p := recover(); p != nil {
			r = cs.ret(fmt.Errorf("panic: %v", p))
		}
	}()
	return cs.ret(cs.s.Put(C.GoBytes(unsafe.Pointer(key), C.int(keyLen)),
		C.GoBytes(unsafe.Pointer(val), C.int(valLen))))
}

//export goStoreDel
func goStoreDel(ctx C.uintptr_t, key *C.uint8_t, keyLen C.size_t) (r C.int) {
	cs := storeOf(ctx)
	defer func() {
		if p := recover(); p != nil {
			r = cs.ret(fmt.Errorf("panic: %v", p))
		}
	}()
	return cs.ret(cs.s.Delete(C.GoBytes(unsafe.Pointer(key), C.int(keyLen))))
}

//export goStoreScan
func goStoreScan(ctx C.uintptr_t, pre *C.uint8_t, preLen C.size_t, fun unsafe.Pointer, arg unsafe.Pointer) (r C.int) {
	cs := storeOf(ctx)
	defer func() {
		if p := recover(); p != nil {
			r = cs.ret(fmt.Errorf("panic: %v", p))
		}
	}()
	var stop C.int
	err := cs.s.Scan(C.GoBytes(unsafe.Pointer(pre), C.int(preLen)), func(k, v []byte) bool {
		kp, vp := cbytes(k), cbytes(v)
		stop = C.mesa_go_scan_row(fun, arg, (*C.uint8_t)(kp), C.size_t(len(k)),
			(*C.uint8_t)(vp), C.size_t(len(v)))
		C.free(kp)
		C.free(vp)
		return stop != 0
	})
	if err != nil {
		return cs.ret(err)
	}
	return stop
}

// cbytes copies b to C memory (never NULL).
func cbytes(b []byte) unsafe.Pointer {
	p := C.malloc(C.size_t(len(b) + 1))
	if len(b) > 0 {
		C.memcpy(p, unsafe.Pointer(&b[0]), C.size_t(len(b)))
	}
	return p
}

// MemoryStore is a volatile Store with savepoints, for tests and
// ephemeral ships.
type MemoryStore struct {
	rows  map[string][]byte
	saved []map[string][]byte // [0] at Begin, then one per Mark
}

func NewMemoryStore() *MemoryStore { return &MemoryStore{rows: map[string][]byte{}} }

func (m *MemoryStore) snap() map[string][]byte {
	c := make(map[string][]byte, len(m.rows))
	for k, v := range m.rows {
		c[k] = v
	}
	return c
}

func (m *MemoryStore) Begin() error {
	if len(m.saved) != 0 {
		return errors.New("mesa: transaction already open")
	}
	m.saved = append(m.saved, m.snap())
	return nil
}

func (m *MemoryStore) Commit() error { m.saved = nil; return nil }

func (m *MemoryStore) Rollback() error {
	if len(m.saved) > 0 {
		m.rows = m.saved[0]
	}
	m.saved = nil
	return nil
}

func (m *MemoryStore) Mark() error { m.saved = append(m.saved, m.snap()); return nil }

func (m *MemoryStore) Rewind() error {
	m.rows = m.saved[len(m.saved)-1]
	m.saved[len(m.saved)-1] = m.snap()
	return nil
}

func (m *MemoryStore) Release() error { m.saved = m.saved[:len(m.saved)-1]; return nil }

func (m *MemoryStore) Get(k []byte) ([]byte, bool, error) {
	v, ok := m.rows[string(k)]
	return v, ok, nil
}

func (m *MemoryStore) Put(k, v []byte) error {
	m.rows[string(k)] = append([]byte(nil), v...)
	return nil
}

func (m *MemoryStore) Delete(k []byte) error { delete(m.rows, string(k)); return nil }

func (m *MemoryStore) Scan(pre []byte, fn func(k, v []byte) bool) error {
	var keys []string
	for k := range m.rows {
		if bytes.HasPrefix([]byte(k), pre) {
			keys = append(keys, k)
		}
	}
	sort.Strings(keys)
	for _, k := range keys {
		v, ok := m.rows[k]
		if ok && fn([]byte(k), v) {
			return nil
		}
	}
	return nil
}

// SQLStore keeps libmesa's state in a SQLite database, in the same table
// and with the same lock as libmesa's own SQLite store and the Python
// binding's, so a database can move between them.
//
// Keep your own tables in the same database, through Conn: written from
// a handler (or inside Node.Do), they commit in the same transaction as
// the message's ack, and each message is processed exactly once.
type SQLStore struct {
	db    *sql.DB
	conn  *sql.Conn
	lock  *os.File
	depth int
}

// OpenSQLite opens (creating if need be) the database at path with the
// database/sql SQLite driver named driver ("sqlite" for
// modernc.org/sqlite, "sqlite3" for mattn/go-sqlite3); import the
// driver yourself. It locks path+".lock" so that two processes can never
// run the same ship.
func OpenSQLite(driver, path string) (*SQLStore, error) {
	lock, err := os.OpenFile(path+".lock", os.O_RDWR|os.O_CREATE, 0o600)
	if err != nil {
		return nil, err
	}
	if err := syscall.Flock(int(lock.Fd()), syscall.LOCK_EX|syscall.LOCK_NB); err != nil {
		lock.Close()
		return nil, fmt.Errorf("mesa: %s is in use by another process", path)
	}
	db, err := sql.Open(driver, path)
	if err != nil {
		lock.Close()
		return nil, err
	}
	//  one connection: BEGIN and SAVEPOINT must all go to the same one
	db.SetMaxOpenConns(1)
	conn, err := db.Conn(context.Background())
	if err == nil {
		for _, q := range []string{
			"PRAGMA journal_mode=WAL",
			"PRAGMA synchronous=FULL",
			"CREATE TABLE IF NOT EXISTS mesa_kv (k BLOB PRIMARY KEY, v BLOB NOT NULL) WITHOUT ROWID",
		} {
			if _, err = conn.ExecContext(context.Background(), q); err != nil {
				break
			}
		}
	}
	if err != nil {
		if conn != nil {
			conn.Close()
		}
		db.Close()
		lock.Close()
		return nil, err
	}
	return &SQLStore{db: db, conn: conn, lock: lock}, nil
}

// Conn is the store's connection. Use it from the Node's goroutine (in
// handlers, Node.Do, or before Run); inside an event it is in that
// event's transaction.
func (s *SQLStore) Conn() *sql.Conn { return s.conn }

// Close closes the database and releases the lock.
func (s *SQLStore) Close() error {
	err := s.conn.Close()
	s.db.Close()
	s.lock.Close()
	return err
}

func (s *SQLStore) exec(q string, args ...any) error {
	_, err := s.conn.ExecContext(context.Background(), q, args...)
	return err
}

func (s *SQLStore) Begin() error  { return s.exec("BEGIN IMMEDIATE") }
func (s *SQLStore) Commit() error { return s.exec("COMMIT") }

func (s *SQLStore) Rollback() error {
	s.depth = 0
	return s.exec("ROLLBACK")
}

func (s *SQLStore) Mark() error {
	s.depth++
	return s.exec(fmt.Sprintf("SAVEPOINT m%d", s.depth))
}

func (s *SQLStore) Rewind() error { return s.exec(fmt.Sprintf("ROLLBACK TO m%d", s.depth)) }

func (s *SQLStore) Release() error {
	err := s.exec(fmt.Sprintf("RELEASE m%d", s.depth))
	s.depth--
	return err
}

func (s *SQLStore) Get(k []byte) ([]byte, bool, error) {
	var v []byte
	err := s.conn.QueryRowContext(context.Background(),
		"SELECT v FROM mesa_kv WHERE k = ?", k).Scan(&v)
	if err == sql.ErrNoRows {
		return nil, false, nil
	}
	if v == nil {
		v = []byte{}
	}
	return v, err == nil, err
}

func (s *SQLStore) Put(k, v []byte) error {
	if v == nil {
		v = []byte{}
	}
	return s.exec("INSERT OR REPLACE INTO mesa_kv VALUES (?, ?)", k, v)
}

func (s *SQLStore) Delete(k []byte) error { return s.exec("DELETE FROM mesa_kv WHERE k = ?", k) }

func (s *SQLStore) Scan(pre []byte, fn func(k, v []byte) bool) error {
	rows, err := s.conn.QueryContext(context.Background(),
		"SELECT k, v FROM mesa_kv WHERE substr(k, 1, ?) = ? ORDER BY k", len(pre), pre)
	if err != nil {
		return err
	}
	//  all rows first: fn may write
	type kv struct{ k, v []byte }
	var all []kv
	for rows.Next() {
		var r kv
		if err := rows.Scan(&r.k, &r.v); err != nil {
			rows.Close()
			return err
		}
		all = append(all, r)
	}
	rows.Close()
	if err := rows.Err(); err != nil {
		return err
	}
	for _, r := range all {
		if fn(r.k, r.v) {
			break
		}
	}
	return nil
}

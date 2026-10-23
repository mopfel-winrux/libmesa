// Package mesa lets a Go program act as an Urbit ship: poke and watch
// gall agents, remote-scry their namespaces, and be poked, watched and
// scried in turn, over Urbit's mesa protocol.
//
// It is a cgo binding to libmesa (`zig build` in the repository root makes
// the static libraries it links). The engine is sans-IO; this package
// drives it from one goroutine with Go's own UDP socket, timers and HTTP
// client, and keeps its state in a Store the application can share, so
// that each message is processed exactly once.
//
// Nouns going in are plain Go values (see Value); nouns coming out are
// borrowed Noun handles, valid only during the callback that got them.
package mesa

/*
#cgo CFLAGS: -I${SRCDIR}/../../zig-out/include
#cgo LDFLAGS: ${SRCDIR}/../../zig-out/lib/libmesa.a ${SRCDIR}/../../zig-out/lib/libmesa-urcrypt.a ${SRCDIR}/../../zig-out/lib/libmesa-sqlite.a -lm -lpthread
#include "shim.h"
*/
import "C"

import (
	"bytes"
	"fmt"
	"math/big"
	"strconv"
	"strings"
	"time"
	"unsafe"
)

// none is MESA_NONE: not a noun.
const none = C.mesa_noun(^uint64(0))

// Noun is a borrowed handle to a libmesa noun. It is valid only during
// the callback that received it; copy out what you need (Jam it, or read
// its atoms) before returning.
type Noun struct{ h C.mesa_noun }

func (n Noun) IsCell() bool { return bool(C.mesa_is_cell(n.h)) }
func (n Noun) IsAtom() bool { return bool(C.mesa_is_atom(n.h)) }

// Head and Tail of a cell; on an atom they return the zero Noun (atom 0).
func (n Noun) Head() Noun {
	if !n.IsCell() {
		return Noun{}
	}
	return Noun{C.mesa_head(n.h)}
}

func (n Noun) Tail() Noun {
	if !n.IsCell() {
		return Noun{}
	}
	return Noun{C.mesa_tail(n.h)}
}

// At returns the noun at tree address axe (1 is the noun itself, 2 its
// head, 3 its tail, ...).
func (n Noun) At(axe uint64) (Noun, bool) {
	r := C.mesa_at(C.uint64_t(axe), n.h)
	if r == none {
		return Noun{}, false
	}
	return Noun{r}, true
}

// Uint64 is the atom's value, if it is an atom that fits.
func (n Noun) Uint64() (uint64, bool) {
	var v C.uint64_t
	if !n.IsAtom() || !bool(C.mesa_atom_get_u64(n.h, &v)) {
		return 0, false
	}
	return uint64(v), true
}

// Bytes is the atom's value, little-endian, without trailing zeros.
func (n Noun) Bytes() []byte {
	if !n.IsAtom() {
		return nil
	}
	len := C.mesa_met3(n.h)
	if len == 0 {
		return []byte{}
	}
	buf := make([]byte, len)
	C.mesa_atom_get_bytes(n.h, len, (*C.uint8_t)(unsafe.Pointer(&buf[0])))
	return buf
}

// Big is the atom's value.
func (n Noun) Big() *big.Int {
	b := n.Bytes()
	for i, j := 0, len(b)-1; i < j; i, j = i+1, j-1 {
		b[i], b[j] = b[j], b[i]
	}
	return new(big.Int).SetBytes(b)
}

// Text is the atom as a cord (UTF-8 text).
func (n Noun) Text() string { return string(n.Bytes()) }

// Equal compares structurally.
func (n Noun) Equal(m Noun) bool { return bool(C.mesa_eq(n.h, m.h)) }

// Jam serializes the noun (+jam), e.g. to keep it past the callback.
func (n Noun) Jam() []byte {
	var len C.size_t
	p := C.mesa_jam_bytes(n.h, &len)
	defer C.mesa_free(unsafe.Pointer(p))
	return C.GoBytes(unsafe.Pointer(p), C.int(len))
}

// List collects the items of a null-terminated list.
func (n Noun) List() []Noun {
	var out []Noun
	for ; n.IsCell(); n = n.Tail() {
		out = append(out, n.Head())
	}
	return out
}

// Path renders a path noun as "/a/b/c".
func (n Noun) Path() string {
	var b strings.Builder
	for _, s := range n.List() {
		b.WriteByte('/')
		b.WriteString(s.Text())
	}
	if b.Len() == 0 {
		return "/"
	}
	return b.String()
}

// Tape reads a tape (a list of bytes) as a string.
func (n Noun) Tape() string {
	var b []byte
	for _, c := range n.List() {
		v, _ := c.Uint64()
		b = append(b, byte(v))
	}
	return string(b)
}

// String renders the noun for logs: cells as [a b], printable atoms as
// %cord or 'text', others in decimal.
func (n Noun) String() string {
	var b strings.Builder
	n.write(&b, 0)
	return b.String()
}

func (n Noun) write(b *strings.Builder, depth int) {
	if depth > 64 {
		b.WriteString("...")
		return
	}
	if n.IsCell() {
		b.WriteByte('[')
		n.Head().write(b, depth+1)
		for t := n.Tail(); ; t = t.Tail() {
			b.WriteByte(' ')
			if !t.IsCell() {
				t.write(b, depth+1)
				break
			}
			t.Head().write(b, depth+1)
		}
		b.WriteByte(']')
		return
	}
	if v, ok := n.Uint64(); ok && v == 0 {
		b.WriteByte('0')
		return
	}
	raw := n.Bytes()
	if len(raw) == 1 && raw[0] >= 'a' && raw[0] <= 'z' {
		b.WriteString("%" + string(raw))
		return
	}
	if printable(raw) {
		if term(raw) {
			b.WriteString("%" + string(raw))
		} else {
			b.WriteString(strconv.Quote(string(raw)))
		}
		return
	}
	b.WriteString(n.Big().String())
}

func printable(b []byte) bool {
	if len(b) < 2 {
		return false
	}
	for _, c := range b {
		if c < 0x20 || c > 0x7e {
			return false
		}
	}
	return true
}

func term(b []byte) bool {
	for i, c := range b {
		if !(c >= 'a' && c <= 'z' || (i > 0 && (c == '-' || c >= '0' && c <= '9'))) {
			return false
		}
	}
	return true
}

// Jammed is a noun serialized with +jam (see Noun.Jam); as a Value it is
// the noun itself.
type Jammed []byte

// Value is a noun to send, as a plain Go value:
//
//	nil              ~ (0)
//	bool             a loobean: true is 0, false is 1
//	uint, int, ...   an atom (negative numbers are an error)
//	string           a cord (@t)
//	[]byte           an atom, little-endian
//	*big.Int         an atom
//	Ship             an @p
//	Cell             a cell; Tuple(a, b, c) is [a b c]
//	Noun             a borrowed noun, passed through
//	Jammed           a jammed noun, cued
//
// Lists, paths and tapes are cells; see Tuple, List, Path, Tape, Some.
type Value any

// Cell is a cell of two Values.
type Cell struct{ H, T Value }

// Tuple is a right-associated cell: Tuple(a, b, c) is [a [b c]], Hoon's
// [a b c].
func Tuple(head Value, rest ...Value) Value {
	if len(rest) == 0 {
		return head
	}
	return Cell{head, Tuple(rest[0], rest[1:]...)}
}

// List is a null-terminated list: List(a, b) is [a b ~].
func List(items ...Value) Value {
	var v Value
	for i := len(items) - 1; i >= 0; i-- {
		v = Cell{items[i], v}
	}
	return v
}

// Path is a path from "/a/b/c": [%a %b %c ~].
//
// Empty segments are kept, as stab keeps them: "/g/x/0/app//1/foo" has an
// empty one after the agent.
func Path(path string) Value {
	path = strings.TrimPrefix(path, "/")
	if path == "" {
		return List()
	}
	var segs []Value
	for _, s := range strings.Split(path, "/") {
		segs = append(segs, s)
	}
	return List(segs...)
}

// Tape is a string as a tape (a list of bytes).
func Tape(s string) Value {
	items := make([]Value, len(s))
	for i := 0; i < len(s); i++ {
		items[i] = uint64(s[i])
	}
	return List(items...)
}

// Some is [~ v], a unit with a value.
func Some(v Value) Value { return Cell{nil, v} }

// daUnixEpoch is ~1970.1.1 in @da seconds.
var daUnixEpoch = new(big.Int).SetUint64(0x8000000cce9e0d80)

// Da is t as an @da (Urbit date).
func Da(t time.Time) *big.Int {
	sec := new(big.Int).Add(daUnixEpoch, big.NewInt(t.Unix()))
	frac := new(big.Int).Lsh(big.NewInt(int64(t.Nanosecond())), 64)
	frac.Div(frac, big.NewInt(1e9))
	return sec.Lsh(sec, 64).Or(sec, frac)
}

// Time reads an @da.
func Time(da Noun) time.Time {
	d := da.Big()
	frac := new(big.Int).And(d, new(big.Int).SetUint64(^uint64(0)))
	sec := new(big.Int).Rsh(d, 64)
	sec.Sub(sec, daUnixEpoch)
	ns := frac.Mul(frac, big.NewInt(1e9))
	ns.Rsh(ns, 64)
	return time.Unix(sec.Int64(), ns.Int64())
}

// build makes a new noun (an owned reference) from v.
func build(v Value) (C.mesa_noun, error) {
	switch x := v.(type) {
	case nil:
		return C.mesa_atom_u64(0), nil
	case bool:
		if x {
			return C.mesa_atom_u64(0), nil
		}
		return C.mesa_atom_u64(1), nil
	case uint64:
		return C.mesa_atom_u64(C.uint64_t(x)), nil
	case uint:
		return C.mesa_atom_u64(C.uint64_t(x)), nil
	case uint32:
		return C.mesa_atom_u64(C.uint64_t(x)), nil
	case uint16:
		return C.mesa_atom_u64(C.uint64_t(x)), nil
	case uint8:
		return C.mesa_atom_u64(C.uint64_t(x)), nil
	case int, int64, int32, int16, int8:
		i := toInt64(x)
		if i < 0 {
			return none, fmt.Errorf("mesa: negative atom %d", i)
		}
		return C.mesa_atom_u64(C.uint64_t(i)), nil
	case string:
		return atomBytes([]byte(x)), nil
	case []byte:
		return atomBytes(x), nil
	case *big.Int:
		if x.Sign() < 0 {
			return none, fmt.Errorf("mesa: negative atom %s", x)
		}
		b := x.Bytes()
		for i, j := 0, len(b)-1; i < j; i, j = i+1, j-1 {
			b[i], b[j] = b[j], b[i]
		}
		return atomBytes(b), nil
	case Ship:
		if x.Hi == 0 {
			return C.mesa_atom_u64(C.uint64_t(x.Lo)), nil
		}
		b := make([]byte, 16)
		for i := 0; i < 8; i++ {
			b[i] = byte(x.Lo >> (8 * i))
			b[8+i] = byte(x.Hi >> (8 * i))
		}
		return atomBytes(bytes.TrimRight(b, "\x00")), nil
	case Cell:
		h, err := build(x.H)
		if err != nil {
			return none, err
		}
		t, err := build(x.T)
		if err != nil {
			C.mesa_unref(h)
			return none, err
		}
		return C.mesa_cell(h, t), nil
	case Noun:
		return C.mesa_ref(x.h), nil
	case Jammed:
		if len(x) == 0 {
			return none, fmt.Errorf("mesa: empty jam")
		}
		h := C.mesa_cue_bytes(C.size_t(len(x)), (*C.uint8_t)(unsafe.Pointer(&x[0])))
		if h == none {
			return none, fmt.Errorf("mesa: bad jam")
		}
		return h, nil
	}
	return none, fmt.Errorf("mesa: can't make a noun of %T", v)
}

func toInt64(v any) int64 {
	switch x := v.(type) {
	case int:
		return int64(x)
	case int64:
		return x
	case int32:
		return int64(x)
	case int16:
		return int64(x)
	case int8:
		return int64(x)
	}
	return -1
}

func atomBytes(b []byte) C.mesa_noun {
	if len(b) == 0 {
		return C.mesa_atom_u64(0)
	}
	return C.mesa_atom_bytes(C.size_t(len(b)), (*C.uint8_t)(unsafe.Pointer(&b[0])))
}

// Tang renders an error noun [tag tang] (a nack's naxplanation) as
// lines: the tag, then each tank flattened as Hoon's +ram does.
func Tang(err Noun) []string {
	if !err.IsCell() {
		return nil
	}
	lines := []string{err.Head().Text()}
	for _, t := range err.Tail().List() {
		lines = append(lines, tank(t, 0))
	}
	return lines
}

func tank(t Noun, depth int) string {
	if depth > 32 || !t.IsCell() || t.Head().IsCell() {
		return ""
	}
	switch t.Head().Text() {
	case "leaf":
		return t.Tail().Tape()
	case "rose", "palm":
		pre := t.Tail().Head()
		var sep, opn, cls string
		if t.Head().Text() == "rose" { // [p=sep q=open r=close]
			sep, opn, cls = pre.Head().Tape(), pre.Tail().Head().Tape(), pre.Tail().Tail().Tape()
		} else { // [p=sep q=open r=open s=close]
			sep = pre.Head().Tape()
			opn = pre.Tail().Head().Tape() + pre.Tail().Tail().Head().Tape()
			cls = pre.Tail().Tail().Tail().Tape()
		}
		var parts []string
		for _, k := range t.Tail().Tail().List() {
			parts = append(parts, tank(k, depth+1))
		}
		return opn + strings.Join(parts, sep) + cls
	}
	return ""
}

// Nack is an error noun we got back for a message: its tag and the
// lines of its tang.
type Nack struct {
	Tag   string
	Lines []string
}

func (e *Nack) Error() string {
	if len(e.Lines) == 0 {
		return "nack %" + e.Tag
	}
	return "nack %" + e.Tag + ": " + strings.Join(e.Lines, "; ")
}

func nackOf(err Noun) *Nack {
	lines := Tang(err)
	if len(lines) == 0 {
		return &Nack{}
	}
	return &Nack{Tag: lines[0], Lines: lines[1:]}
}

// errorNoun is the [tag tang] we send for a Go error.
func errorNoun(err error) C.mesa_noun {
	tag, lines := "go-error", []string{err.Error()}
	if n, ok := err.(*Nack); ok {
		tag, lines = n.Tag, n.Lines
	}
	tanks := make([]Value, len(lines))
	for i, l := range lines {
		tanks[i] = Cell{"leaf", Tape(l)}
	}
	h, e := build(Cell{tag, List(tanks...)})
	if e != nil {
		return C.mesa_atom_u64(0)
	}
	return h
}

func unref(h C.mesa_noun) { C.mesa_unref(h) }

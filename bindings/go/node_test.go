package mesa

import (
	"context"
	"errors"
	"path/filepath"
	"strings"
	"testing"
	"time"

	_ "modernc.org/sqlite"
)

//  Two fake galaxies talking over localhost (fake galaxy ~g is at
//  127.0.0.1:31337+g); each test uses its own pair so ports don't clash.

type event struct {
	kind  string
	her   Ship
	bone  uint64
	text  string
	nack  *Nack
	value string
}

type recorder struct {
	BaseApp
	ev    chan event
	poke  func(n *Node, her Ship, bone uint64, agent, mark string, noun Noun) error
	scrys map[string]string
	facts map[uint64]uint64 // subscriber bone -> facts left to give
}

func newRecorder() *recorder { return &recorder{ev: make(chan event, 64), scrys: map[string]string{}} }

func (r *recorder) Poke(n *Node, her Ship, bone uint64, agent, mark string, noun Noun) error {
	if r.poke != nil {
		return r.poke(n, her, bone, agent, mark, noun)
	}
	if agent != "echo" {
		return Drop
	}
	r.ev <- event{kind: "poke", her: her, bone: bone, text: mark + " " + noun.String()}
	if noun.Text() == "bad" {
		return &Nack{Tag: "bad-poke", Lines: []string{"no thanks"}}
	}
	return nil
}

func (r *recorder) Watch(n *Node, her Ship, bone uint64, agent, path string) error {
	if agent != "echo" {
		return Drop
	}
	r.ev <- event{kind: "watch", her: her, bone: bone, text: path}
	//  a fact right away, in the same transaction as the watch-ack
	return n.Fact(her, bone, "noun", Tuple("hello", path))
}

func (r *recorder) Leave(n *Node, her Ship, bone uint64, agent string) {
	r.ev <- event{kind: "leave", her: her, bone: bone}
}

func (r *recorder) Fact(n *Node, her Ship, bone uint64, mark string, noun Noun) {
	r.ev <- event{kind: "fact", her: her, bone: bone, text: mark + " " + noun.String()}
}

func (r *recorder) Done(n *Node, her Ship, bone uint64, err *Nack) {
	r.ev <- event{kind: "done", her: her, bone: bone, nack: err}
}

func (r *recorder) Corked(n *Node, her Ship, bone uint64) {
	r.ev <- event{kind: "corked", her: her, bone: bone}
}

func (r *recorder) Scry(n *Node, her Ship, path, mark string, noun Noun, ok bool) {
	r.ev <- event{kind: "scry", her: her, text: path, value: mark + " " + noun.String()}
}

func (r *recorder) Peek(n *Node, her *Ship, path string) (string, Value, bool) {
	if strings.HasSuffix(path, "/greeting") {
		return "noun", Tuple("hi", 42), true
	}
	return "", nil, false
}

func (r *recorder) next(t *testing.T, kind string) event {
	t.Helper()
	deadline := time.After(20 * time.Second)
	for {
		select {
		case e := <-r.ev:
			if e.kind == kind {
				return e
			}
			t.Logf("(skipping %s %s)", e.kind, e.text)
		case <-deadline:
			t.Fatalf("no %s within 20s", kind)
		}
	}
}

func start(t *testing.T, g uint64, st Store, app App) (*Node, func()) {
	t.Helper()
	n, err := Open(Config{Identity: FakeIdentity(Ship{Lo: g}), Store: st, App: app})
	if err != nil {
		t.Fatalf("open ~%s: %v", Ship{Lo: g}, err)
	}
	ctx, cancel := context.WithCancel(context.Background())
	go n.Run(ctx)
	return n, func() { cancel(); n.Close() }
}

func TestShipsAndNouns(t *testing.T) {
	s := MustShip("~sampel-palnet")
	if s.String() != "~sampel-palnet" || s.Rank() != Planet {
		t.Fatalf("ship %s rank %d", s, s.Rank())
	}
	if _, err := ParseShip("~notaship"); err == nil {
		t.Fatal("parsed ~notaship")
	}
	tm := time.Date(2026, 10, 22, 12, 0, 0, 500_000_000, time.UTC)
	h, err := build(Tuple(Da(tm), "x", List(1, 2), true, Ship{Lo: 0x10000}))
	if err != nil {
		t.Fatal(err)
	}
	defer unref(h)
	n := Noun{h}
	if got := Time(n.Head()); !got.Equal(tm) {
		t.Fatalf("da round trip: %v", got)
	}
	if got := n.Tail().String(); got != `[%x [1 2 0] 0 65536]` {
		t.Fatalf("String: %s", got)
	}
}

func TestPokeWatchScry(t *testing.T) {
	ra, rb := newRecorder(), newRecorder()
	a, stopA := start(t, 201, NewMemoryStore(), ra)
	defer stopA()
	b, stopB := start(t, 202, NewMemoryStore(), rb)
	defer stopB()
	her := b.Ship()

	//  poke: ack
	var bone uint64
	err := a.Do(func() (err error) {
		if bone, err = a.OpenFlow(her); err != nil {
			return err
		}
		return a.Poke(her, bone, "echo", "noun", "hi")
	})
	if err != nil {
		t.Fatal(err)
	}
	if e := rb.next(t, "poke"); e.her != a.Ship() || e.text != `noun %hi` {
		t.Fatalf("poke: %+v", e)
	}
	if e := ra.next(t, "done"); e.bone != bone || e.nack != nil {
		t.Fatalf("done: %+v", e)
	}

	//  poke: nack, with our tang
	a.Do(func() error { return a.Poke(her, bone, "echo", "noun", "bad") })
	e := ra.next(t, "done")
	if e.nack == nil || e.nack.Tag != "bad-poke" || len(e.nack.Lines) != 1 || e.nack.Lines[0] != "no thanks" {
		t.Fatalf("nack: %+v %v", e, e.nack)
	}

	//  watch: watch-ack, then the fact
	var sub uint64
	a.Do(func() (err error) {
		if sub, err = a.OpenFlow(her); err != nil {
			return err
		}
		return a.Watch(her, sub, "echo", "/updates/1")
	})
	if e := rb.next(t, "watch"); e.text != "/updates/1" {
		t.Fatalf("watch: %+v", e)
	}
	if e := ra.next(t, "fact"); e.bone != sub || e.text != `noun [%hello %/updates/1]` && e.text != `noun [%hello "/updates/1"]` {
		t.Fatalf("fact: %+v", e)
	}

	//  leave, then cork
	a.Do(func() error { return a.Leave(her, sub, "echo") })
	rb.next(t, "leave")
	a.Do(func() error { return a.Cork(her, sub) })
	if e := ra.next(t, "corked"); e.bone != sub {
		t.Fatalf("corked: %+v", e)
	}

	//  remote scry
	a.Do(func() error { return a.Scry(her, "/g/x/1/echo//1/greeting") })
	if e := ra.next(t, "scry"); e.value != `noun [%hi 42]` {
		t.Fatalf("scry: %+v", e)
	}

	//  the API refuses calls from outside the loop
	if _, err := a.OpenFlow(her); !errors.Is(err, ErrNotOnLoop) {
		t.Fatalf("off-loop OpenFlow: %v", err)
	}
}

func TestDropThenAck(t *testing.T) {
	ra, rb := newRecorder(), newRecorder()
	drops := 0
	rb.poke = func(n *Node, her Ship, bone uint64, agent, mark string, noun Noun) error {
		if drops < 2 {
			drops++
			return Drop // e.g. the agent isn't running yet
		}
		rb.ev <- event{kind: "poke"}
		return nil
	}
	a, stopA := start(t, 203, NewMemoryStore(), ra)
	defer stopA()
	b, stopB := start(t, 204, NewMemoryStore(), rb)
	defer stopB()

	a.Do(func() error {
		bone, err := a.OpenFlow(b.Ship())
		if err != nil {
			return err
		}
		return a.Poke(b.Ship(), bone, "late", "noun", 1)
	})
	rb.next(t, "poke")
	if e := ra.next(t, "done"); e.nack != nil {
		t.Fatalf("done: %+v", e)
	}
	if drops != 2 {
		t.Fatalf("drops %d", drops)
	}
}

// exactly once: the receiver counts pokes in its own table, in the same
// transaction as each ack, and survives a restart
func TestSQLStore(t *testing.T) {
	dir := t.TempDir()
	path := filepath.Join(dir, "b.db")

	open := func() (*SQLStore, *recorder) {
		st, err := OpenSQLite("sqlite", path)
		if err != nil {
			t.Fatal(err)
		}
		if _, err := st.Conn().ExecContext(context.Background(),
			"CREATE TABLE IF NOT EXISTS hits (n INTEGER)"); err != nil {
			t.Fatal(err)
		}
		r := newRecorder()
		r.poke = func(n *Node, her Ship, bone uint64, agent, mark string, noun Noun) error {
			_, err := st.Conn().ExecContext(context.Background(), "INSERT INTO hits VALUES (1)")
			r.ev <- event{kind: "poke"}
			return err
		}
		return st, r
	}
	count := func(st *SQLStore) (k int) {
		st.Conn().QueryRowContext(context.Background(), "SELECT count(*) FROM hits").Scan(&k)
		return
	}

	if _, err := OpenSQLite("sqlite", path); err != nil {
		t.Fatal(err)
	} // (left open: the lock is ours until the test ends; reopen below must fail)
	if _, err := OpenSQLite("sqlite", path); err == nil {
		t.Fatal("opened a locked database twice")
	}

	path = filepath.Join(dir, "c.db")
	ra := newRecorder()
	a, stopA := start(t, 205, NewMemoryStore(), ra)
	defer stopA()

	st, rb := open()
	b, stopB := start(t, 206, st, rb)
	var bone uint64
	a.Do(func() (err error) {
		bone, err = a.OpenFlow(b.Ship())
		for i := 0; err == nil && i < 3; i++ {
			err = a.Poke(b.Ship(), bone, "count", "noun", i)
		}
		return err
	})
	for i := 0; i < 3; i++ {
		ra.next(t, "done")
	}
	stopB()
	if k := count(st); k != 3 {
		t.Fatalf("hits %d", k)
	}
	st.Close()

	//  again, from the same database
	st, rb = open()
	defer st.Close()
	b, stopB = start(t, 206, st, rb)
	defer stopB()
	a.Do(func() error { return a.Poke(b.Ship(), bone, "count", "noun", 9) })
	ra.next(t, "done")
	if k := count(st); k != 4 {
		t.Fatalf("hits after restart %d", k)
	}
}

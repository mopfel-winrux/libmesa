package mesa

import (
	"context"
	"errors"
	"path/filepath"
	"testing"
	"time"
)

// a publisher that accepts watches on %feed and remembers their bones
type feed struct {
	BaseApp
	ev chan event
}

func (f *feed) Watch(n *Node, her Ship, bone uint64, agent, path string) error {
	if agent != "feed" {
		return Drop
	}
	f.ev <- event{kind: "watch", her: her, bone: bone, text: path}
	return n.Fact(her, bone, "noun", "welcome")
}

func (f *feed) Leave(n *Node, her Ship, bone uint64, agent string) {
	f.ev <- event{kind: "leave", her: her, bone: bone}
}

func (f *feed) Poke(n *Node, her Ship, bone uint64, agent, mark string, noun Noun) error {
	if agent != "feed" {
		return Drop
	}
	f.ev <- event{kind: "poke", her: her, bone: bone, text: noun.Text()}
	if noun.Text() == "bad" {
		return &Nack{Tag: "nope"}
	}
	return nil
}

func next(t *testing.T, ch chan event, kind string) event {
	t.Helper()
	r := &recorder{ev: ch}
	return r.next(t, kind)
}

// gather waits for one event of each kind, in any order
func gather(t *testing.T, ch chan event, kinds ...string) map[string]event {
	t.Helper()
	got := map[string]event{}
	deadline := time.After(20 * time.Second)
	for len(got) < len(kinds) {
		select {
		case e := <-ch:
			for _, k := range kinds {
				if e.kind == k {
					got[k] = e
				}
			}
		case <-deadline:
			t.Fatalf("got %v of %v within 20s", got, kinds)
		}
	}
	return got
}

func subsFor(ch chan event) *Subs {
	return &Subs{
		OnFact: func(n *Node, s Sub, mark string, noun Noun) {
			ch <- event{kind: "fact", her: s.Ship, text: s.Path, value: noun.Text()}
		},
		OnWatch: func(n *Node, s Sub, err *Nack) {
			ch <- event{kind: "watched", her: s.Ship, text: s.Path, nack: err}
		},
		OnKick: func(n *Node, s Sub) { ch <- event{kind: "kicked", her: s.Ship, text: s.Path} },
	}
}

// subscriptions survive kicks (resubscribing, with a backoff) and
// restarts (the flow is remembered), and end cleanly
func TestSubsResubscribe(t *testing.T) {
	pub := &feed{ev: make(chan event, 64)}
	b, stopB := start(t, 211, NewMemoryStore(), pub)
	defer stopB()

	path := filepath.Join(t.TempDir(), "a.db")
	ev := make(chan event, 64)
	open := func() (*Node, *SQLStore, *Subs, func()) {
		st, err := OpenSQLite("sqlite", path)
		if err != nil {
			t.Fatal(err)
		}
		subs := subsFor(ev)
		subs.Backoff = 2 * time.Second
		n, err := Open(Config{Identity: FakeIdentity(Ship{Lo: 210}), Store: st, Helpers: []Helper{subs}})
		if err != nil {
			t.Fatal(err)
		}
		ctx, cancel := context.WithCancel(context.Background())
		go n.Run(ctx)
		return n, st, subs, func() { cancel(); n.Close(); st.Close() }
	}
	a, _, subs, stopA := open()

	if err := a.Do(func() error { return subs.Subscribe(a, b.Ship(), "feed", "/news") }); err != nil {
		t.Fatal(err)
	}
	w1 := next(t, pub.ev, "watch")
	if w1.text != "/news" || w1.her != a.Ship() {
		t.Fatalf("watch: %+v", w1)
	}
	//  the fact comes with the watch-ack, in either order
	got := gather(t, ev, "watched", "fact")
	if got["watched"].nack != nil || got["fact"].value != "welcome" {
		t.Fatalf("watch-ack and fact: %+v", got)
	}
	//  subscribing twice is a no-op
	a.Do(func() error { return subs.Subscribe(a, b.Ship(), "feed", "/news") })

	//  kicked: right back on a new flow
	b.Do(func() error { return b.Kick(a.Ship(), w1.bone) })
	gather(t, ev, "kicked")
	w2 := next(t, pub.ev, "watch")
	if w2.bone == w1.bone {
		t.Fatalf("resubscribed on the old flow %d", w1.bone)
	}
	next(t, ev, "fact")

	//  kicked again at once: after a backoff
	kicked := time.Now()
	b.Do(func() error { return b.Kick(a.Ship(), w2.bone) })
	w3 := next(t, pub.ev, "watch")
	if d := time.Since(kicked); d < 900*time.Millisecond {
		t.Fatalf("resubscribed after %v, no backoff", d)
	}
	next(t, ev, "fact")

	//  restart: facts on the flow still reach OnFact
	stopA()
	a, _, subs, stopA = open()
	defer func() { stopA() }()
	if got := subs.List(); len(got) != 1 || got[0].Path != "/news" {
		t.Fatalf("subs after restart: %+v", got)
	}
	b.Do(func() error { return b.Fact(a.Ship(), w3.bone, "noun", "later") })
	if e := next(t, ev, "fact"); e.value != "later" {
		t.Fatalf("fact after restart: %+v", e)
	}

	//  unsubscribe: leave, and the flow closes
	a.Do(func() error { return subs.Unsubscribe(a, b.Ship(), "feed", "/news") })
	if e := next(t, pub.ev, "leave"); e.bone != w3.bone {
		t.Fatalf("leave: %+v", e)
	}
	if len(subs.List()) != 0 {
		t.Fatal("still subscribed")
	}
}

// a refused watch ends the subscription
func TestSubsNack(t *testing.T) {
	c, stopC := start(t, 213, NewMemoryStore(), refuser{})
	defer stopC()

	ev := make(chan event, 64)
	subs := subsFor(ev)
	a, err := Open(Config{Identity: FakeIdentity(Ship{Lo: 212}), Store: NewMemoryStore(), Helpers: []Helper{subs}})
	if err != nil {
		t.Fatal(err)
	}
	ctx, cancel := context.WithCancel(context.Background())
	go a.Run(ctx)
	defer func() { cancel(); a.Close() }()

	a.Do(func() error { return subs.Subscribe(a, c.Ship(), "feed", "/x") })
	if e := next(t, ev, "watched"); e.nack == nil || e.nack.Tag != "go-error" {
		t.Fatalf("watch-nack: %+v", e)
	}
	if len(subs.List()) != 0 {
		t.Fatal("refused subscription kept")
	}
}

type refuser struct{ BaseApp }

func (refuser) Watch(n *Node, her Ship, bone uint64, agent, path string) error {
	return errors.New("no")
}

// pokes go out in order on one flow per agent, numbered, across restarts
func TestPokes(t *testing.T) {
	pub := &feed{ev: make(chan event, 64)}
	b, stopB := start(t, 215, NewMemoryStore(), pub)
	defer stopB()

	ev := make(chan event, 64)
	st := NewMemoryStore()
	pk := &Pokes{OnDone: func(n *Node, her Ship, agent string, seq uint64, err *Nack) {
		ev <- event{kind: "done", her: her, bone: seq, nack: err, text: agent}
	}}
	a, err := Open(Config{Identity: FakeIdentity(Ship{Lo: 214}), Store: st, Helpers: []Helper{pk}})
	if err != nil {
		t.Fatal(err)
	}
	ctx, cancel := context.WithCancel(context.Background())
	go a.Run(ctx)

	var seqs []uint64
	a.Do(func() error {
		for _, s := range []string{"one", "bad", "three"} {
			seq, err := pk.Poke(a, b.Ship(), "feed", "noun", s)
			if err != nil {
				return err
			}
			seqs = append(seqs, seq)
		}
		return nil
	})
	if len(seqs) != 3 || seqs[0] != 1 || seqs[2] != 3 {
		t.Fatalf("seqs %v", seqs)
	}
	var bones []uint64
	for _, want := range []string{"one", "bad", "three"} {
		e := next(t, pub.ev, "poke")
		if e.text != want {
			t.Fatalf("poke %q, want %q", e.text, want)
		}
		bones = append(bones, e.bone)
	}
	if bones[0] != bones[1] || bones[1] != bones[2] {
		t.Fatalf("pokes on several flows: %v", bones)
	}
	for i := uint64(1); i <= 3; i++ {
		e := next(t, ev, "done")
		if e.bone != i || (i == 2) != (e.nack != nil) {
			t.Fatalf("done %d: %+v", i, e)
		}
	}
	bone, _ := pk.Bone(b.Ship(), "feed")
	cancel()
	a.Close()

	//  again, on the same store: the same flow, numbering on
	pk2 := &Pokes{OnDone: pk.OnDone}
	a, err = Open(Config{Identity: FakeIdentity(Ship{Lo: 214}), Store: st, Helpers: []Helper{pk2}})
	if err != nil {
		t.Fatal(err)
	}
	ctx, cancel = context.WithCancel(context.Background())
	go a.Run(ctx)
	defer func() { cancel(); a.Close() }()
	if b2, ok := pk2.Bone(b.Ship(), "feed"); !ok || b2 != bone {
		t.Fatalf("bone after restart %d %v, was %d", b2, ok, bone)
	}
	var seq uint64
	a.Do(func() (err error) { seq, err = pk2.Poke(a, b.Ship(), "feed", "noun", "four"); return })
	if seq != 4 {
		t.Fatalf("seq after restart %d", seq)
	}
	next(t, pub.ev, "poke")
	if e := next(t, ev, "done"); e.bone != 4 || e.nack != nil {
		t.Fatalf("done 4: %+v", e)
	}
}

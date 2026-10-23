package tlon

import (
	"context"
	"math/big"
	"testing"
	"time"

	mesa "github.com/mopfel-winrux/libmesa/bindings/go"
	_ "modernc.org/sqlite"
)

type ev struct {
	kind, text, id string
	her            mesa.Ship
	nack           *mesa.Nack
}

type probe struct {
	mesa.BaseApp
	ch chan ev
}

func (p *probe) Fact(n *mesa.Node, her mesa.Ship, bone uint64, mark string, noun mesa.Noun) {
	p.ch <- ev{kind: "fact", text: mark + " " + noun.String()}
}

func (p *probe) Done(n *mesa.Node, her mesa.Ship, bone uint64, err *mesa.Nack) {
	p.ch <- ev{kind: "done", nack: err}
}

var held = map[chan ev][]ev{}

// wait is the next [kind] event on ch; others are kept for later waits
func wait(t *testing.T, ch chan ev, kind string) ev {
	t.Helper()
	for i, e := range held[ch] {
		if e.kind == kind {
			held[ch] = append(held[ch][:i:i], held[ch][i+1:]...)
			return e
		}
	}
	deadline := time.After(20 * time.Second)
	for {
		select {
		case e := <-ch:
			if e.kind == kind {
				return e
			}
			held[ch] = append(held[ch], e)
		case <-deadline:
			t.Fatalf("no %s within 20s", kind)
		}
	}
}

func node(t *testing.T, g uint64, dms *DMs, app mesa.App) *mesa.Node {
	t.Helper()
	n, err := mesa.Open(mesa.Config{Identity: mesa.FakeIdentity(mesa.Ship{Lo: g}),
		Store: mesa.NewMemoryStore(), App: app, Helpers: []mesa.Helper{dms}})
	if err != nil {
		t.Fatal(err)
	}
	ctx, cancel := context.WithCancel(context.Background())
	go n.Run(ctx)
	t.Cleanup(func() { cancel(); n.Close() })
	return n
}

// two ships DM each other: one echoes, and a resent message isn't
// delivered twice
func TestDMRoundTrip(t *testing.T) {
	ach, bch := make(chan ev, 64), make(chan ev, 64)

	var bd DMs
	bd.OnMessage = func(n *mesa.Node, m Message) error {
		bch <- ev{kind: "msg", text: m.Text, id: m.ID, her: m.From}
		_, err := bd.Send(n, m.From, "you said: "+m.Text)
		return err
	}
	ad := &DMs{
		OnMessage: func(n *mesa.Node, m Message) error {
			ach <- ev{kind: "msg", text: m.Text, her: m.From}
			return nil
		},
		OnSent: func(n *mesa.Node, her mesa.Ship, id string, err *mesa.Nack) {
			ach <- ev{kind: "sent", id: id, nack: err}
		},
	}
	pa := &probe{ch: ach}
	a := node(t, 217, ad, pa)
	b := node(t, 218, &bd, nil)

	var id string
	if err := a.Do(func() (err error) {
		id, err = ad.Send(a, b.Ship(), "hello\nthere")
		return
	}); err != nil {
		t.Fatal(err)
	}
	m := wait(t, bch, "msg")
	if m.text != "hello\nthere" || m.her != a.Ship() || m.id != id {
		t.Fatalf("b got %+v (id %s)", m, id)
	}
	if s := wait(t, ach, "sent"); s.id != id || s.nack != nil {
		t.Fatalf("sent: %+v", s)
	}
	if r := wait(t, ach, "msg"); r.text != "you said: hello\nthere" || r.her != b.Ship() {
		t.Fatalf("reply: %+v", r)
	}

	//  Tlon resending the same message on a new flow: acked, not redelivered
	a.Do(func() error {
		bone, err := a.OpenFlow(b.Ship())
		if err != nil {
			return err
		}
		us := a.Ship()
		da, _ := new(big.Int).SetString(id, 10)
		essay := mesa.Tuple(mesa.Tuple(Story("hello\nthere"), us, da), mesa.Tuple("chat", nil), nil, nil)
		return a.Poke(b.Ship(), bone, Agent, Mark, mesa.Tuple(mesa.Tuple(us, da), mesa.Tuple("add", essay, nil)))
	})
	if d := wait(t, ach, "done"); d.nack != nil {
		t.Fatalf("resend: %+v", d)
	}
	select {
	case e := <-bch:
		t.Fatalf("delivered twice: %+v", e)
	case <-time.After(300 * time.Millisecond):
	}

	//  Tlon's version check
	a.Do(func() error {
		bone, err := a.OpenFlow(b.Ship())
		if err != nil {
			return err
		}
		return a.Watch(b.Ship(), bone, Agent, VersionPath)
	})
	if f := wait(t, ach, "fact"); f.text != "noun 2" {
		t.Fatalf("version: %+v", f)
	}
}

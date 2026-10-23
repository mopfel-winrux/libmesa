// Package tlon speaks Tlon's direct-message protocol (tloncorp/tlon-apps,
// desk/app/chat.hoon) over a mesa.Node: DMs is a mesa.Helper that runs
// as agent %chat, takes %chat-dm-diff pokes, answers Tlon's version
// check, and sends DMs on one ordered flow per ship.
//
//	dms := &tlon.DMs{OnMessage: func(n *mesa.Node, m tlon.Message) error {
//		_, err := dms.Send(n, m.From, "you said: "+m.Text)
//		return err
//	}}
//	n, _ := mesa.Open(mesa.Config{..., Helpers: []mesa.Helper{dms}})
//
// A message is handed to OnMessage once: the dedupe record commits in
// the same transaction as its ack, and so does anything OnMessage writes
// or sends.
package tlon

import (
	"encoding/binary"
	"math/big"
	"strings"
	"time"

	mesa "github.com/mopfel-winrux/libmesa/bindings/go"
)

// Agent is the agent Tlon DMs go to.
const Agent = "chat"

// Mark is the mark we send DMs with; we take any chat-dm-diff.
const Mark = "chat-dm-diff-2"

// Version is the %chat-dms protocol version we answer Tlon's version
// check (a watch on VersionPath) with.
const (
	Version     = 2
	VersionPath = "/~/negotiate/version/chat-dms"
)

// Message is a DM we got.
type Message struct {
	From  mesa.Ship
	Sent  time.Time // the author's timestamp
	ID    string    // the sent @da, in decimal: the message's id
	Text  string    // the story's plain text (StoryText)
	Story mesa.Noun // the story itself; borrowed for the call
}

// DMs is a mesa.Helper for Tlon DMs.
type DMs struct {
	mesa.BaseHelper

	// OnMessage: a new DM. Runs in the transaction that acks it; an
	// error nacks it (and undoes its writes). Messages Tlon resends
	// (same author and sent time) are acked without calling it again.
	OnMessage func(n *mesa.Node, m Message) error

	// OnSent: our DM [id] to [her] was delivered (err nil) or refused.
	// Optional.
	OnSent func(n *mesa.Node, her mesa.Ship, id string, err *mesa.Nack)

	pokes  mesa.Pokes
	lastDa time.Time
}

func (d *DMs) Start(n *mesa.Node) error {
	d.pokes.Name = "tlon-dms"
	d.pokes.OnDone = d.done
	return d.pokes.Start(n)
}

func ship16(s mesa.Ship) []byte {
	b := binary.LittleEndian.AppendUint64(nil, s.Lo)
	return binary.LittleEndian.AppendUint64(b, s.Hi)
}

// keys (see mesa.Helper): 'at' ship sent: a message we got;
// 'aT' ship seq: the id of our message [seq] not yet answered
func seenKey(her mesa.Ship, id string) []byte {
	return append(append(append([]byte("at"), ship16(her)...), id...), 0)
}

func sentKey(her mesa.Ship, seq uint64) []byte {
	return binary.BigEndian.AppendUint64(append([]byte("aT"), ship16(her)...), seq)
}

func (d *DMs) HandleWatch(n *mesa.Node, her mesa.Ship, bone uint64, agent, path string) (bool, error) {
	if agent != Agent || path != VersionPath {
		return false, nil
	}
	return true, n.Fact(her, bone, "noun", Version)
}

func (d *DMs) HandlePoke(n *mesa.Node, her mesa.Ship, bone uint64, agent, mark string, noun mesa.Noun) (bool, error) {
	if agent != Agent {
		return false, nil
	}
	if !strings.HasPrefix(mark, "chat-dm-diff") {
		return true, nil // rsvps, (un)blocks: nothing to do
	}
	//  [id=[author sent] %add essay=[[content author sent] kind meta blob] time]
	delta := noun.Tail()
	if !delta.IsCell() || delta.Head().Text() != "add" {
		return true, nil // deletes, reactions, replies
	}
	memo := delta.Tail().Head().Head()
	sent := memo.Tail().Tail()
	id := sent.Big().String()

	k := seenKey(her, id)
	st := n.Store()
	if _, ok, err := st.Get(k); err != nil || ok {
		return true, err
	}
	if err := st.Put(k, nil); err != nil {
		return true, err
	}
	if d.OnMessage == nil {
		return true, nil
	}
	return true, d.OnMessage(n, Message{
		From:  her,
		Sent:  mesa.Time(sent),
		ID:    id,
		Text:  strings.TrimSpace(StoryText(memo.Head())),
		Story: memo.Head(),
	})
}

// Send DMs [text] to [her] and returns the message's id. Call it on the
// loop (in a handler or Node.Do).
func (d *DMs) Send(n *mesa.Node, her mesa.Ship, text string) (string, error) {
	return d.SendStory(n, her, Story(text))
}

// SendStory DMs a story (see Story).
func (d *DMs) SendStory(n *mesa.Node, her mesa.Ship, story mesa.Value) (string, error) {
	//  a fresh @da per message: it's the message's id
	now := time.Now()
	if !now.After(d.lastDa) {
		now = d.lastDa.Add(time.Millisecond)
	}
	d.lastDa = now
	sent := mesa.Da(now)
	us := n.Ship()
	essay := mesa.Tuple(mesa.Tuple(story, us, sent), mesa.Tuple("chat", nil), nil, nil)
	seq, err := d.pokes.Poke(n, her, Agent, Mark,
		mesa.Tuple(mesa.Tuple(us, sent), mesa.Tuple("add", essay, nil)))
	if err != nil {
		return "", err
	}
	id := sent.String()
	return id, n.Store().Put(sentKey(her, seq), []byte(id))
}

func (d *DMs) done(n *mesa.Node, her mesa.Ship, agent string, seq uint64, err *mesa.Nack) {
	k := sentKey(her, seq)
	v, _, _ := n.Store().Get(k)
	n.Store().Delete(k)
	if d.OnSent != nil {
		d.OnSent(n, her, string(v), err)
	}
}

func (d *DMs) HandleDone(n *mesa.Node, her mesa.Ship, bone uint64, err *mesa.Nack) bool {
	return d.pokes.HandleDone(n, her, bone, err)
}

func (d *DMs) HandleCorked(n *mesa.Node, her mesa.Ship, bone uint64) bool {
	return d.pokes.HandleCorked(n, her, bone)
}

func (d *DMs) HandleBreach(n *mesa.Node, her mesa.Ship) { d.pokes.HandleBreach(n, her) }

// Story is plain text as a Tlon story: one paragraph, lines broken.
func Story(text string) mesa.Value {
	var inl []mesa.Value
	for i, line := range strings.Split(text, "\n") {
		if i > 0 {
			inl = append(inl, mesa.Tuple("break", nil))
		}
		if line != "" {
			inl = append(inl, line)
		}
	}
	if len(inl) == 0 {
		inl = append(inl, "")
	}
	return mesa.List(mesa.Tuple("inline", mesa.List(inl...)))
}

// StoryText is a story's plain text, best effort.
func StoryText(s mesa.Noun) string {
	var out []string
	for _, verse := range s.List() {
		if !verse.IsCell() {
			continue
		}
		switch verse.Head().Text() {
		case "inline":
			if t := inlines(verse.Tail(), 0); t != "" {
				out = append(out, t)
			}
		case "block":
			out = append(out, "["+verse.Tail().Head().Text()+"]")
		}
	}
	return strings.Join(out, "\n")
}

func inlines(l mesa.Noun, depth int) string {
	var b strings.Builder
	for _, i := range l.List() {
		b.WriteString(inline(i, depth))
	}
	return b.String()
}

func inline(i mesa.Noun, depth int) string {
	if depth > 32 {
		return ""
	}
	if i.IsAtom() {
		return i.Text()
	}
	p := i.Tail()
	switch i.Head().Text() {
	case "italics", "bold", "strike", "blockquote":
		return inlines(p, depth+1)
	case "task":
		return inlines(p.Tail(), depth+1)
	case "inline-code", "code":
		return p.Text()
	case "tag":
		return "#" + p.Text()
	case "ship":
		return shipOf(p.Big()).String()
	case "break":
		return "\n"
	case "link":
		if t := p.Tail().Text(); t != "" {
			return t
		}
		return p.Head().Text()
	case "block":
		return p.Tail().Text()
	}
	return ""
}

func shipOf(a *big.Int) mesa.Ship {
	lo := new(big.Int).And(a, new(big.Int).SetUint64(^uint64(0)))
	return mesa.Ship{Lo: lo.Uint64(), Hi: new(big.Int).Rsh(a, 64).Uint64()}
}

package mesa

import (
	"bytes"
	"encoding/binary"
	"errors"
	"sort"
	"time"
)

// Helper sees requests and responses before the App does, so that
// protocol code (subscriptions, ordered pokes, Tlon DMs) needn't be
// written into every App. Handlers run on the loop, inside the event's
// transaction, like App's. Helpers are tried in Config.Helpers order;
// the first to claim an event (true) gets it, and App doesn't.
//
// A helper keeps its state in the node's Store, under keys that start
// with 'a' (the engine's own keys are upper case), so it commits with
// the engine's and survives restarts. Embed BaseHelper and override
// what you need.
type Helper interface {
	// Start loads the helper's state; it runs once, after the engine
	// opens and before Run.
	Start(n *Node) error

	HandlePoke(n *Node, her Ship, bone uint64, agent, mark string, noun Noun) (bool, error)
	HandleWatch(n *Node, her Ship, bone uint64, agent, path string) (bool, error)
	HandleFact(n *Node, her Ship, bone uint64, mark string, noun Noun) bool
	HandleKick(n *Node, her Ship, bone uint64) bool
	HandleDone(n *Node, her Ship, bone uint64, err *Nack) bool
	HandleCorked(n *Node, her Ship, bone uint64) bool

	// HandleBreach: every helper sees it, then App.
	HandleBreach(n *Node, her Ship)
}

// BaseHelper claims nothing.
type BaseHelper struct{}

func (BaseHelper) Start(*Node) error { return nil }
func (BaseHelper) HandlePoke(*Node, Ship, uint64, string, string, Noun) (bool, error) {
	return false, nil
}
func (BaseHelper) HandleWatch(*Node, Ship, uint64, string, string) (bool, error) {
	return false, nil
}
func (BaseHelper) HandleFact(*Node, Ship, uint64, string, Noun) bool { return false }
func (BaseHelper) HandleKick(*Node, Ship, uint64) bool               { return false }
func (BaseHelper) HandleDone(*Node, Ship, uint64, *Nack) bool        { return false }
func (BaseHelper) HandleCorked(*Node, Ship, uint64) bool             { return false }
func (BaseHelper) HandleBreach(*Node, Ship)                          {}

//  ---------------------------------------------------------------- keys
//
//  Helper keys, shared with the Python binding (one database works with
//  both): 'a', a kind byte, then fields; ships are 16 bytes little-endian,
//  numbers 8 bytes big-endian, strings NUL-terminated.

func shipBytes(s Ship) []byte {
	var b [16]byte
	binary.LittleEndian.PutUint64(b[:8], s.Lo)
	binary.LittleEndian.PutUint64(b[8:], s.Hi)
	return b[:]
}

func shipFrom(b []byte) Ship {
	return Ship{binary.LittleEndian.Uint64(b[:8]), binary.LittleEndian.Uint64(b[8:16])}
}

func u64(b []byte) uint64 { return binary.BigEndian.Uint64(b) }

func putU64(b []byte, v uint64) []byte { return binary.BigEndian.AppendUint64(b, v) }

// key builds 'a' kind fields...: Ship, uint64, string, []byte.
func key(kind byte, fields ...any) []byte {
	k := []byte{'a', kind}
	for _, f := range fields {
		switch f := f.(type) {
		case Ship:
			k = append(k, shipBytes(f)...)
		case uint64:
			k = putU64(k, f)
		case string:
			k = append(append(k, f...), 0)
		case []byte:
			k = append(k, f...)
		}
	}
	return k
}

// cut splits a NUL-terminated string off b.
func cut(b []byte) (string, []byte, bool) {
	i := bytes.IndexByte(b, 0)
	if i < 0 {
		return "", nil, false
	}
	return string(b[:i]), b[i+1:], true
}

//  ---------------------------------------------------------------- Subs

// Sub is a subscription of ours: [Path] on [Ship]'s [Agent].
type Sub struct {
	Ship  Ship
	Agent string
	Path  string
}

type subState byte

const (
	subWatching subState = 1 // watch sent; no ack yet
	subLive     subState = 2 // acked
	subWaiting  subState = 3 // kicked; resubscribing at .at
)

type sub struct {
	Sub
	bone  uint64
	state subState
	tries uint8
	at    uint64 // ms: when to resubscribe (waiting), or since (live)
}

// Subs keeps subscriptions alive: Subscribe watches on a fresh flow, a
// kick (or the publisher's breach) resubscribes after a backoff, and
// Unsubscribe leaves and corks. Facts on its flows go to OnFact, not to
// App.Fact.
type Subs struct {
	BaseHelper

	// OnFact: an update on [s].
	OnFact func(n *Node, s Sub, mark string, noun Noun)

	// OnWatch: [s] was accepted (err nil), or refused (err: their nack;
	// the subscription is over, and won't be retried). Optional.
	OnWatch func(n *Node, s Sub, err *Nack)

	// OnKick: [s] was kicked and will be resubscribed. Optional.
	OnKick func(n *Node, s Sub)

	// Backoff caps the delay between resubscribes after repeated kicks
	// (default 5 minutes). A kick after a minute or more of being live
	// resubscribes right away.
	Backoff time.Duration

	subs  map[Sub]*sub
	bones map[Ship]map[uint64]*sub
	gone  map[Ship]map[uint64]bool // left or kicked flows, until corked
}

func (s *Subs) init() {
	if s.subs == nil {
		s.subs = map[Sub]*sub{}
		s.bones = map[Ship]map[uint64]*sub{}
		s.gone = map[Ship]map[uint64]bool{}
	}
}

func (s *Subs) skey(k Sub) []byte { return key('s', k.Ship, k.Agent, k.Path) }

func (s *Subs) save(n *Node, b *sub) error {
	v := putU64(nil, b.bone)
	v = append(v, byte(b.state), b.tries)
	v = putU64(v, b.at)
	return n.Store().Put(s.skey(b.Sub), v)
}

func (s *Subs) bind(b *sub) {
	m := s.bones[b.Ship]
	if m == nil {
		m = map[uint64]*sub{}
		s.bones[b.Ship] = m
	}
	m[b.bone] = b
}

func (s *Subs) unbind(b *sub) {
	if b.state != subWaiting {
		delete(s.bones[b.Ship], b.bone)
	}
}

func (s *Subs) bury(n *Node, her Ship, bone uint64) error {
	m := s.gone[her]
	if m == nil {
		m = map[uint64]bool{}
		s.gone[her] = m
	}
	m[bone] = true
	return n.Store().Put(key('l', her, bone), nil)
}

func (s *Subs) find(her Ship, bone uint64) *sub { return s.bones[her][bone] }

func (s *Subs) Start(n *Node) error {
	s.init()
	st := n.Store()
	err := st.Scan([]byte("as"), func(k, v []byte) bool {
		if len(k) < 18 || len(v) != 18 {
			return false
		}
		agent, rest, ok1 := cut(k[18:])
		path, _, ok2 := cut(rest)
		if !ok1 || !ok2 {
			return false
		}
		b := &sub{Sub: Sub{shipFrom(k[2:18]), agent, path},
			bone: u64(v), state: subState(v[8]), tries: v[9], at: u64(v[10:])}
		s.subs[b.Sub] = b
		if b.state != subWaiting {
			s.bind(b)
		}
		return false
	})
	if err == nil {
		err = st.Scan([]byte("al"), func(k, v []byte) bool {
			if len(k) == 26 {
				her := shipFrom(k[2:18])
				if s.gone[her] == nil {
					s.gone[her] = map[uint64]bool{}
				}
				s.gone[her][u64(k[18:])] = true
			}
			return false
		})
	}
	if err != nil {
		return err
	}
	for _, b := range s.subs {
		if b.state == subWaiting {
			s.schedule(n, b)
		}
	}
	return nil
}

// Subscribe watches [path] on [her]'s [agent], unless we already do.
// Call it on the loop (in a handler or Node.Do).
func (s *Subs) Subscribe(n *Node, her Ship, agent, path string) error {
	s.init()
	k := Sub{her, agent, path}
	if _, ok := s.subs[k]; ok {
		return nil
	}
	b := &sub{Sub: k}
	if err := s.watch(n, b); err != nil {
		return err
	}
	s.subs[k] = b
	return nil
}

func (s *Subs) watch(n *Node, b *sub) error {
	bone, err := n.OpenFlow(b.Ship)
	if err != nil {
		return err
	}
	if err := n.Watch(b.Ship, bone, b.Agent, b.Path); err != nil {
		return err
	}
	b.bone, b.state = bone, subWatching
	s.bind(b)
	return s.save(n, b)
}

// Unsubscribe leaves [path] on [her]'s [agent] and closes its flow.
func (s *Subs) Unsubscribe(n *Node, her Ship, agent, path string) error {
	s.init()
	k := Sub{her, agent, path}
	b, ok := s.subs[k]
	if !ok {
		return nil
	}
	if b.state != subWaiting {
		if err := n.Leave(her, b.bone, agent); err != nil {
			return err
		}
		if err := n.Cork(her, b.bone); err != nil && !errors.Is(err, ErrState) {
			return err
		}
		if err := s.bury(n, her, b.bone); err != nil {
			return err
		}
		s.unbind(b)
	}
	delete(s.subs, k)
	return n.Store().Delete(s.skey(k))
}

// List is our subscriptions, sorted.
func (s *Subs) List() []Sub {
	out := make([]Sub, 0, len(s.subs))
	for k := range s.subs {
		out = append(out, k)
	}
	sort.Slice(out, func(i, j int) bool {
		a, b := out[i], out[j]
		if a.Ship != b.Ship {
			return a.Ship.Hi < b.Ship.Hi || a.Ship.Hi == b.Ship.Hi && a.Ship.Lo < b.Ship.Lo
		}
		if a.Agent != b.Agent {
			return a.Agent < b.Agent
		}
		return a.Path < b.Path
	})
	return out
}

func (s *Subs) backoff(tries uint8) time.Duration {
	max := s.Backoff
	if max <= 0 {
		max = 5 * time.Minute
	}
	if tries == 0 {
		return 0
	}
	d := time.Second << min(tries-1, 20)
	return min(d, max)
}

// kicked: resubscribe [b] after a backoff (its old flow, if any, is gone)
func (s *Subs) kicked(n *Node, b *sub) error {
	now := nowMs()
	if b.state == subLive && now-b.at >= 60_000 {
		b.tries = 0
	}
	s.unbind(b)
	b.at = now + uint64(s.backoff(b.tries)/time.Millisecond)
	if b.tries < 255 {
		b.tries++
	}
	b.state = subWaiting
	if err := s.save(n, b); err != nil {
		return err
	}
	s.schedule(n, b)
	return nil
}

func (s *Subs) schedule(n *Node, b *sub) {
	now, at := nowMs(), b.at
	var d time.Duration
	if at > now {
		d = time.Duration(at-now) * time.Millisecond
	}
	n.After(d, func() error {
		if s.subs[b.Sub] != b || b.state != subWaiting {
			return nil // unsubscribed, or resubscribed
		}
		if nowMs() < b.at {
			s.schedule(n, b) // kicked again since
			return nil
		}
		return s.watch(n, b)
	})
}

func (s *Subs) HandleFact(n *Node, her Ship, bone uint64, mark string, noun Noun) bool {
	if s.gone[her][bone] {
		return true
	}
	b := s.find(her, bone)
	if b == nil {
		return false
	}
	if s.OnFact != nil {
		s.OnFact(n, b.Sub, mark, noun)
	}
	return true
}

func (s *Subs) HandleKick(n *Node, her Ship, bone uint64) bool {
	if s.gone[her][bone] {
		return true
	}
	b := s.find(her, bone)
	if b == nil {
		return false
	}
	//  gall closes a kicked subscription's flow; a new one resubscribes
	if err := n.Cork(her, bone); err != nil && !errors.Is(err, ErrState) {
		logf("mesa: cork %s %d: %v", her, bone, err)
	}
	if err := s.bury(n, her, bone); err != nil {
		logf("mesa: subs: %v", err)
	}
	if s.OnKick != nil {
		s.OnKick(n, b.Sub)
	}
	if err := s.kicked(n, b); err != nil {
		logf("mesa: subs: %v", err)
	}
	return true
}

func (s *Subs) HandleDone(n *Node, her Ship, bone uint64, err *Nack) bool {
	if s.gone[her][bone] {
		return true
	}
	b := s.find(her, bone)
	if b == nil {
		return false
	}
	if b.state != subWatching {
		return true
	}
	if err != nil {
		//  refused: over for good
		s.unbind(b)
		delete(s.subs, b.Sub)
		n.Store().Delete(s.skey(b.Sub))
		if e := n.Cork(her, bone); e != nil && !errors.Is(e, ErrState) {
			logf("mesa: cork %s %d: %v", her, bone, e)
		}
		s.bury(n, her, bone)
	} else {
		b.state, b.at = subLive, nowMs()
		s.save(n, b)
	}
	if s.OnWatch != nil {
		s.OnWatch(n, b.Sub, err)
	}
	return true
}

func (s *Subs) HandleCorked(n *Node, her Ship, bone uint64) bool {
	if !s.gone[her][bone] {
		return s.find(her, bone) != nil
	}
	delete(s.gone[her], bone)
	n.Store().Delete(key('l', her, bone))
	return true
}

func (s *Subs) HandleBreach(n *Node, her Ship) {
	for bone := range s.gone[her] {
		n.Store().Delete(key('l', her, bone))
	}
	delete(s.gone, her)
	for _, b := range s.bones[her] {
		if err := s.kicked(n, b); err != nil {
			logf("mesa: subs: %v", err)
		}
	}
	delete(s.bones, her)
}

//  ---------------------------------------------------------------- Pokes

type pokeFlow struct {
	her   Ship
	agent string
	bone  uint64
	next  uint64 // seq of the next poke (from 1)
	acked uint64 // pokes answered
}

// Pokes sends pokes on one flow per (ship, agent), so they arrive in
// the order sent, and remembers the flow across restarts. Acks and nacks
// go to OnDone, not App.Done.
type Pokes struct {
	BaseHelper

	// Name keeps two Pokes on one node apart (e.g. "tlon-dms").
	Name string

	// OnDone: poke [seq] (as Poke returned it) to [her]'s [agent] was
	// acked (err nil) or nacked. A breach nacks every unanswered poke
	// with tag "breach". Optional.
	OnDone func(n *Node, her Ship, agent string, seq uint64, err *Nack)

	flows map[Ship]map[string]*pokeFlow
	bones map[Ship]map[uint64]*pokeFlow
}

func (p *Pokes) init() {
	if p.flows == nil {
		p.flows = map[Ship]map[string]*pokeFlow{}
		p.bones = map[Ship]map[uint64]*pokeFlow{}
	}
}

func (p *Pokes) pkey(her Ship, agent string) []byte { return key('p', p.Name, her, agent) }

func (p *Pokes) bind(f *pokeFlow) {
	if p.flows[f.her] == nil {
		p.flows[f.her] = map[string]*pokeFlow{}
		p.bones[f.her] = map[uint64]*pokeFlow{}
	}
	p.flows[f.her][f.agent] = f
	p.bones[f.her][f.bone] = f
}

func (p *Pokes) save(n *Node, f *pokeFlow) error {
	return n.Store().Put(p.pkey(f.her, f.agent), putU64(putU64(putU64(nil, f.bone), f.next), f.acked))
}

func (p *Pokes) Start(n *Node) error {
	p.init()
	pre := key('p', p.Name)
	return n.Store().Scan(pre, func(k, v []byte) bool {
		rest := k[len(pre):]
		if len(rest) < 17 || len(v) != 24 {
			return false
		}
		agent, _, ok := cut(rest[16:])
		if !ok {
			return false
		}
		p.bind(&pokeFlow{her: shipFrom(rest), agent: agent, bone: u64(v), next: u64(v[8:]), acked: u64(v[16:])})
		return false
	})
}

// Poke pokes [her]'s [agent] with [mark] and [v], on their flow (opened
// if new); seq numbers the pokes to each (ship, agent) from 1. Call it on
// the loop (in a handler or Node.Do).
func (p *Pokes) Poke(n *Node, her Ship, agent, mark string, v Value) (seq uint64, err error) {
	p.init()
	f := p.flows[her][agent]
	if f == nil {
		bone, err := n.OpenFlow(her)
		if err != nil {
			return 0, err
		}
		f = &pokeFlow{her: her, agent: agent, bone: bone, next: 1}
	}
	if err := n.Poke(her, f.bone, agent, mark, v); err != nil {
		return 0, err
	}
	seq = f.next
	f.next++
	if err := p.save(n, f); err != nil {
		f.next--
		return 0, err
	}
	p.bind(f)
	return seq, nil
}

// Bone is the flow we poke [her]'s [agent] on, if we have one.
func (p *Pokes) Bone(her Ship, agent string) (uint64, bool) {
	f := p.flows[her][agent]
	if f == nil {
		return 0, false
	}
	return f.bone, true
}

func (p *Pokes) HandleDone(n *Node, her Ship, bone uint64, err *Nack) bool {
	f := p.bones[her][bone]
	if f == nil {
		return false
	}
	f.acked++
	if e := p.save(n, f); e != nil {
		logf("mesa: pokes: %v", e)
	}
	if p.OnDone != nil {
		p.OnDone(n, her, f.agent, f.acked, err)
	}
	return true
}

func (p *Pokes) HandleCorked(n *Node, her Ship, bone uint64) bool {
	return p.bones[her][bone] != nil
}

func (p *Pokes) HandleBreach(n *Node, her Ship) {
	for agent, f := range p.flows[her] {
		n.Store().Delete(p.pkey(her, agent))
		for seq := f.acked + 1; seq < f.next; seq++ {
			if p.OnDone != nil {
				p.OnDone(n, her, agent, seq, &Nack{Tag: "breach"})
			}
		}
	}
	delete(p.flows, her)
	delete(p.bones, her)
}

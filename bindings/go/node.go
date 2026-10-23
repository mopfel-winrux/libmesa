package mesa

// #include "shim.h"
import "C"

import (
	"context"
	"errors"
	"fmt"
	"log"
	"net"
	"net/http"
	"net/netip"
	"os"
	"runtime"
	"runtime/cgo"
	"sync"
	"sync/atomic"
	"time"
	"unsafe"
)

// Logf logs the binding's messages; replace it to redirect them.
var Logf = log.Printf

func logf(format string, args ...any) { Logf(format, args...) }

// Error is a libmesa error code.
type Error int

const (
	ErrStore    Error = C.MESA_E_STORE    // store failed while opening
	ErrFresh    Error = C.MESA_E_FRESH    // empty store, and not told we breached
	ErrIdentity Error = C.MESA_E_IDENTITY // store belongs to another ship/life/rift
	ErrArg      Error = C.MESA_E_ARG      // bad argument (unknown flow, bad path, ...)
	ErrState    Error = C.MESA_E_STATE    // the flow is closing or corked, or a transaction is (or isn't) open
	ErrFailed   Error = C.MESA_E_FAILED   // engine stopped after a store failure
)

func (e Error) Error() string {
	switch e {
	case ErrStore:
		return "mesa: store failed"
	case ErrFresh:
		return "mesa: this identity has run before, but its store is empty " +
			"(set Config.Breached only if it has breached)"
	case ErrIdentity:
		return "mesa: the store belongs to another ship, life or rift"
	case ErrArg:
		return "mesa: bad argument"
	case ErrState:
		return "mesa: not now (flow closing or corked, or transaction state)"
	case ErrFailed:
		return "mesa: engine stopped after a store failure; reopen it"
	}
	return fmt.Sprintf("mesa: error %d", int(e))
}

func check(rc C.mesa_err) error {
	if rc == C.MESA_OK {
		return nil
	}
	return Error(rc)
}

var (
	// Drop, returned from Poke, Watch or Plea: the request isn't ours
	// (an agent we don't run). It gets no answer and its writes are
	// undone; the peer will resend it.
	Drop = errors.New("mesa: drop")

	// Later, returned from Plea: we'll answer with Node.Answer.
	Later = errors.New("mesa: later")

	// ErrNotOnLoop: the engine was called from outside the Node's loop.
	// Use Node.Do.
	ErrNotOnLoop = errors.New("mesa: call the engine from a handler or Node.Do")

	// ErrClosed: the node is closed.
	ErrClosed = errors.New("mesa: node closed")
)

// App is what an application does with the network. Embed BaseApp and
// override what you need.
//
// Handlers run on the Node's goroutine, inside the event's store
// transaction: writes to the Store (SQLStore.Conn) commit together with
// the ack. Nouns they get are borrowed: valid only until they return.
type App interface {
	// Poke: [her] poked our [agent] with [mark] and [noun]. Return nil to
	// ack, Drop if we don't run [agent], or any error to nack (a *Nack
	// sets the tag and tang sent back).
	Poke(n *Node, her Ship, bone uint64, agent, mark string, noun Noun) error

	// Watch: [her] subscribes to [path] on our [agent]. Return nil to
	// accept (then give facts with Node.Fact on [bone]), Drop, or an
	// error to refuse.
	Watch(n *Node, her Ship, bone uint64, agent, path string) error

	// Leave: [her] unsubscribed.
	Leave(n *Node, her Ship, bone uint64, agent string)

	// Plea: any other request (vanes other than gall). Return nil, Drop,
	// Later (then answer with Node.Answer), or an error to nack.
	Plea(n *Node, her Ship, bone uint64, vane string, path, payload Noun) error

	// Fact: an update on our subscription [bone].
	Fact(n *Node, her Ship, bone uint64, mark string, noun Noun)

	// Kick: our subscription [bone] was ended by [her].
	Kick(n *Node, her Ship, bone uint64)

	// Boon: any other response on our flow [bone].
	Boon(n *Node, her Ship, bone uint64, payload Noun)

	// Done: our message on [bone] was acked (err nil) or nacked.
	Done(n *Node, her Ship, bone uint64, err *Nack)

	// Corked: our flow [bone] is closed for good.
	Corked(n *Node, her Ship, bone uint64)

	// Scry: the answer to Node.Scry; ok is false if it was dropped
	// because [her] breached.
	Scry(n *Node, her Ship, path, mark string, noun Noun, ok bool)

	// Peek: serve our namespace. [her] is the reader of an encrypted
	// read, nil for a public one. Return ok false if there's nothing at
	// [path]. A path's value must never change once it has one.
	Peek(n *Node, her *Ship, path string) (mark string, value Value, ok bool)

	// Breach: [her] breached; every flow with them is gone.
	Breach(n *Node, her Ship)
}

// BaseApp drops every request and ignores every response.
type BaseApp struct{}

func (BaseApp) Poke(*Node, Ship, uint64, string, string, Noun) error { return Drop }
func (BaseApp) Watch(*Node, Ship, uint64, string, string) error      { return Drop }
func (BaseApp) Leave(*Node, Ship, uint64, string)                    {}
func (BaseApp) Plea(*Node, Ship, uint64, string, Noun, Noun) error   { return Drop }
func (BaseApp) Fact(*Node, Ship, uint64, string, Noun)               {}
func (BaseApp) Kick(*Node, Ship, uint64)                             {}
func (BaseApp) Boon(*Node, Ship, uint64, Noun)                       {}
func (BaseApp) Done(*Node, Ship, uint64, *Nack)                      {}
func (BaseApp) Corked(*Node, Ship, uint64)                           {}
func (BaseApp) Scry(*Node, Ship, string, string, Noun, bool)         {}
func (BaseApp) Peek(*Node, *Ship, string) (string, Value, bool)      { return "", nil, false }
func (BaseApp) Breach(*Node, Ship)                                   {}

// Config opens a Node.
type Config struct {
	Identity *Identity
	Store    Store
	App      App // nil: BaseApp

	// Port is the UDP port to bind; 0 means a galaxy's own port, or an
	// ephemeral one for everyone else.
	Port int

	// Breached allows starting with an empty store: only if this
	// identity never ran before, or breached since it last ran. Comets
	// and fake ships don't need it.
	Breached bool

	// NoPing stops the 25s ping that keeps our galaxy's route to us.
	NoPing bool

	// AzimuthURL is where to look up keys (default AzimuthURL);
	// DNSDomain where galaxies are (default "urbit.org": ~zod is at
	// zod.urbit.org:13337).
	AzimuthURL string
	DNSDomain  string
	HTTPClient *http.Client

	// Trace logs every datagram (also set by $MESA_TRACE).
	Trace bool
}

// Node is a ship on the network.
//
// The engine is single-threaded: everything that touches it runs on the
// goroutine that calls Run (handlers, and functions passed to Do). Its
// API methods return ErrNotOnLoop from anywhere else.
type Node struct {
	cfg   Config
	app   App
	id    *Identity
	our   Ship
	fake  bool
	trace bool
	az    string
	dns   string
	http  *http.Client

	eng   *C.mesa_engine
	store *cstore
	h     cgo.Handle
	conn  *net.UDPConn

	tid    atomic.Int64 // the OS thread the engine runs on (0: none)
	inHook bool         // inside the engine's own transaction

	mu     sync.Mutex
	jobs   []func()
	wake   chan struct{}
	pkts   chan packet
	asking map[Ship]bool
	gal    map[uint8]galaxy
	galAsk map[uint8]bool
	closed bool
	done   chan struct{} // closed when Run returns
	stop   chan struct{}
}

type packet struct {
	data []byte
	from netip.AddrPort
}

type galaxy struct {
	addr netip.AddrPort
	when time.Time
}

// Open binds the socket, checks our keys against azimuth (for galaxies,
// stars and planets), and opens the engine on the store.
func Open(cfg Config) (*Node, error) {
	if cfg.Identity == nil || cfg.Store == nil {
		return nil, errors.New("mesa: Config needs an Identity and a Store")
	}
	n := &Node{
		cfg:    cfg,
		app:    cfg.App,
		id:     cfg.Identity,
		our:    cfg.Identity.Ship(),
		fake:   cfg.Identity.fake,
		trace:  cfg.Trace || os.Getenv("MESA_TRACE") != "",
		az:     cfg.AzimuthURL,
		dns:    cfg.DNSDomain,
		http:   cfg.HTTPClient,
		wake:   make(chan struct{}, 1),
		pkts:   make(chan packet, 1024),
		asking: map[Ship]bool{},
		gal:    map[uint8]galaxy{},
		galAsk: map[uint8]bool{},
		stop:   make(chan struct{}),
	}
	if n.app == nil {
		n.app = BaseApp{}
	}
	if n.az == "" {
		n.az = AzimuthURL
	}
	if n.dns == "" {
		n.dns = "urbit.org"
	}
	if n.http == nil {
		n.http = &http.Client{Timeout: 30 * time.Second}
	}

	//  the socket first: opening the engine resumes flows, which may send
	port := cfg.Port
	if port == 0 && n.our.Rank() == Galaxy {
		port = 13337 + int(n.our.Lo)
		if n.fake {
			port = 31337 + int(n.our.Lo)
		}
	}
	ip := net.IPv4zero
	if n.fake {
		ip = net.IPv4(127, 0, 0, 1)
	}
	conn, err := net.ListenUDP("udp4", &net.UDPAddr{IP: ip, Port: port})
	if err != nil {
		return nil, err
	}
	n.conn = conn

	//  our own point: our keys must match, and it names our sponsor
	ide := n.id.c
	var sponsor Ship
	hasSponsor := false
	if r := n.our.Rank(); !n.fake && r <= Planet {
		pot, err := FetchPoint(n.http, n.az, n.our)
		if err != nil {
			conn.Close()
			return nil, fmt.Errorf("mesa: can't find %s on azimuth: %w", n.our, err)
		}
		var pass [65]byte
		C.mesa_ident_pass(&ide, (*C.uint8_t)(unsafe.Pointer(&pass[0])))
		if pot.Pass != pass || pot.Life != uint64(ide.life) {
			conn.Close()
			return nil, fmt.Errorf("mesa: our keys don't match azimuth (life %d there, %d here)",
				pot.Life, uint64(ide.life))
		}
		if ide.rift == C.MESA_RIFT_UNKNOWN {
			ide.rift = C.uint64_t(pot.Rift)
		}
		if pot.HasSponsor {
			sponsor, hasSponsor = pot.Sponsor, true
		}
	}

	n.store = newCStore(cfg.Store)
	n.h = cgo.NewHandle(n)

	var con C.mesa_config
	con.ident = ide
	con.store = n.store.c
	C.mesa_go_hooks(&con.hooks, C.uintptr_t(n.h))
	con.our_sponsor = sponsor.c()
	con.has_sponsor = C.bool(hasSponsor)
	con.breached = C.bool(cfg.Breached || n.fake)
	con.no_ping = C.bool(cfg.NoPing)
	con.fake = C.bool(n.fake)
	con.now = C.uint64_t(nowMs())

	n.pin()
	rc := C.mesa_engine_open(&con, &n.eng)
	n.unpin()
	if err := check(C.mesa_err(rc)); err != nil {
		conn.Close()
		n.store.free()
		n.h.Delete()
		return nil, err
	}
	return n, nil
}

// Ship is who we are.
func (n *Node) Ship() Ship { return n.our }

// Addr is the local UDP address.
func (n *Node) Addr() net.Addr { return n.conn.LocalAddr() }

func nowMs() uint64 { return uint64(time.Now().UnixMilli()) }

// post queues fn to run on the loop.
func (n *Node) post(fn func()) bool {
	n.mu.Lock()
	if n.closed {
		n.mu.Unlock()
		return false
	}
	n.jobs = append(n.jobs, fn)
	n.mu.Unlock()
	select {
	case n.wake <- struct{}{}:
	default:
	}
	return true
}

// Do runs fn on the Node's goroutine, in one store transaction, and
// returns its error: if fn fails, its writes and messages are undone.
// Call it from any goroutine but the Node's own (there, call the API
// directly), or before Run, when it runs fn right away.
func (n *Node) Do(fn func() error) error {
	n.mu.Lock()
	running, closed := n.done != nil, n.closed
	n.mu.Unlock()
	if closed {
		return ErrClosed
	}
	if !running {
		n.pin()
		defer n.unpin()
		return n.txn(fn)
	}
	res := make(chan error, 1)
	if !n.post(func() { res <- n.txn(fn) }) {
		return ErrClosed
	}
	select {
	case err := <-res:
		return err
	case <-n.stop:
		return ErrClosed
	}
}

// txn runs fn in an application transaction (on the loop).
func (n *Node) txn(fn func() error) (err error) {
	if n.inHook {
		return fn()
	}
	if err := check(C.mesa_txn_begin(n.eng)); err != nil {
		return err
	}
	defer func() {
		if p := recover(); p != nil {
			err = fmt.Errorf("mesa: panic: %v", p)
		}
		if err != nil {
			C.mesa_txn_abort(n.eng)
			return
		}
		err = check(C.mesa_txn_commit(n.eng))
	}()
	return fn()
}

// Run drives the node until ctx is done or Close is called.
func (n *Node) Run(ctx context.Context) error {
	n.mu.Lock()
	if n.closed || n.done != nil {
		n.mu.Unlock()
		return ErrClosed
	}
	n.done = make(chan struct{})
	n.mu.Unlock()
	defer close(n.done)

	n.pin()
	defer n.unpin()

	go n.read()

	timer := time.NewTimer(time.Hour)
	defer timer.Stop()
	for {
		n.arm(timer)
		select {
		case <-ctx.Done():
			return ctx.Err()
		case <-n.stop:
			return nil
		case p, ok := <-n.pkts:
			if !ok {
				return nil
			}
			n.hear(p)
			for i := 0; i < 255; i++ {
				select {
				case p, ok = <-n.pkts:
					if !ok {
						return nil
					}
					n.hear(p)
					continue
				default:
				}
				break
			}
		case <-n.wake:
			n.mu.Lock()
			jobs := n.jobs
			n.jobs = nil
			n.mu.Unlock()
			for _, j := range jobs {
				j()
			}
		case <-timer.C:
		}
		if d := uint64(C.mesa_engine_deadline(n.eng)); d != ^uint64(0) && d <= nowMs() {
			C.mesa_engine_wake(n.eng, C.uint64_t(nowMs()))
		}
	}
}

func (n *Node) arm(t *time.Timer) {
	d := uint64(C.mesa_engine_deadline(n.eng))
	if d == ^uint64(0) {
		t.Reset(time.Hour)
		return
	}
	now := nowMs()
	if d <= now {
		t.Reset(0)
		return
	}
	t.Reset(time.Duration(d-now) * time.Millisecond)
}

func (n *Node) read() {
	for {
		buf := make([]byte, 2048)
		k, from, err := n.conn.ReadFromUDPAddrPort(buf)
		if err != nil {
			if errors.Is(err, net.ErrClosed) {
				close(n.pkts)
				return
			}
			continue
		}
		select {
		case n.pkts <- packet{buf[:k], from}:
		default: // the engine resends
		}
	}
}

func (n *Node) hear(p packet) {
	if len(p.data) == 0 {
		return
	}
	if n.trace {
		logf("mesa: hear %d from %s", len(p.data), p.from)
	}
	a4 := p.from.Addr().Unmap().As4()
	var lane C.mesa_lane
	lane.kind = C.MESA_LANE_IP
	lane.ip = C.uint32_t(uint32(a4[0])<<24 | uint32(a4[1])<<16 | uint32(a4[2])<<8 | uint32(a4[3]))
	lane.port = C.uint16_t(p.from.Port())
	C.mesa_engine_hear(n.eng, lane, (*C.uint8_t)(unsafe.Pointer(&p.data[0])),
		C.size_t(len(p.data)), C.uint64_t(nowMs()))
}

// Close stops the loop (if running) and closes the engine and socket.
// It doesn't close the Store.
func (n *Node) Close() error {
	n.mu.Lock()
	if n.closed {
		n.mu.Unlock()
		return nil
	}
	n.closed = true
	done := n.done
	n.mu.Unlock()
	close(n.stop)
	if done != nil {
		<-done
	}
	n.conn.Close()
	C.mesa_engine_close(n.eng)
	n.store.free()
	n.h.Delete()
	return nil
}

func nodeOf(ctx C.uintptr_t) *Node { return cgo.Handle(ctx).Value().(*Node) }

//  ---------------------------------------------------------------- hooks

// hook runs fn as the engine's hook: on the loop, inside its transaction.
func (n *Node) hook(name string, fn func()) {
	n.inHook = true
	defer func() {
		n.inHook = false
		if p := recover(); p != nil {
			logf("mesa: panic in %s: %v", name, p)
		}
	}()
	fn()
}

//export goSend
func goSend(ctx C.uintptr_t, lane *C.mesa_lane, buf *C.uint8_t, len C.size_t) {
	n := nodeOf(ctx)
	var to netip.AddrPort
	switch lane.kind {
	case C.MESA_LANE_GALAXY:
		to = n.galaxy(uint8(lane.galaxy))
	case C.MESA_LANE_IP:
		if lane.ip == 0 || lane.port == 0 {
			break
		}
		ip := uint32(lane.ip)
		addr := netip.AddrFrom4([4]byte{byte(ip >> 24), byte(ip >> 16), byte(ip >> 8), byte(ip)})
		if n.fake {
			addr = netip.AddrFrom4([4]byte{127, 0, 0, 1})
		}
		to = netip.AddrPortFrom(addr, uint16(lane.port))
	}
	if n.trace {
		logf("mesa: send %d to %v", int(len), to)
	}
	if !to.IsValid() {
		return
	}
	//  never blocks for long; on error the engine resends
	n.conn.WriteToUDPAddrPort(C.GoBytes(unsafe.Pointer(buf), C.int(len)), to)
}

// galaxy is where galaxy g is, if we know; finding out happens off the
// loop, and a datagram meanwhile is dropped (the engine resends).
func (n *Node) galaxy(g uint8) netip.AddrPort {
	if n.fake {
		return netip.AddrPortFrom(netip.AddrFrom4([4]byte{127, 0, 0, 1}), 31337+uint16(g))
	}
	hit, ok := n.gal[g]
	if ok && time.Since(hit.when) < 5*time.Minute {
		return hit.addr
	}
	if !n.galAsk[g] {
		n.galAsk[g] = true
		name := Ship{Lo: uint64(g)}.String()[1:] + "." + n.dns
		go func() {
			ips, err := net.DefaultResolver.LookupNetIP(context.Background(), "ip4", name)
			n.post(func() {
				n.galAsk[g] = false
				if err != nil || len(ips) == 0 {
					logf("mesa: can't find galaxy %s: %v", name, err)
					return
				}
				n.gal[g] = galaxy{netip.AddrPortFrom(ips[0].Unmap(), 13337+uint16(g)), time.Now()}
			})
		}()
	}
	return hit.addr // stale or nothing
}

//export goNeedPoint
func goNeedPoint(ctx C.uintptr_t, who *C.mesa_ship) {
	n := nodeOf(ctx)
	her := shipOf(who)
	if n.fake {
		var ide C.mesa_ident
		C.mesa_ident_fake(her.c(), &ide)
		var c C.mesa_point
		c.who, c.rift, c.life = her.c(), 0, 1
		C.mesa_ident_pass(&ide, &c.pass[0])
		n.post(func() { n.point(&c) })
		return
	}
	if n.asking[her] {
		return
	}
	n.asking[her] = true
	go func() {
		pot, err := FetchPoint(n.http, n.az, her)
		if err != nil && !errors.Is(err, ErrNotFound) {
			logf("mesa: azimuth %s: %v", her, err)
		}
		n.post(func() {
			delete(n.asking, her)
			if pot == nil {
				C.mesa_engine_point_missing(n.eng, her.c())
				return
			}
			c := pot.c()
			n.point(&c)
		})
	}()
}

func (n *Node) point(c *C.mesa_point) {
	C.mesa_engine_point(n.eng, c)
}

//export goPlea
func goPlea(ctx C.uintptr_t, her *C.mesa_ship, bone C.uint64_t, vane, path, payload C.mesa_noun, errOut *C.mesa_noun) (res C.int) {
	n := nodeOf(ctx)
	who := shipOf(her)
	b := uint64(bone)
	var err error
	n.inHook = true
	defer func() {
		n.inHook = false
		if p := recover(); p != nil {
			err = fmt.Errorf("panic: %v", p)
			logf("mesa: panic in plea: %v", p)
		}
		switch {
		case err == nil:
			res = C.MESA_ACK
		case errors.Is(err, Drop):
			res = C.MESA_DROP
		case errors.Is(err, Later):
			res = C.MESA_LATER
		default:
			*errOut = errorNoun(err)
			res = C.MESA_NACK
		}
	}()

	var req C.mesa_gall_request
	switch C.mesa_gall_request_parse(vane, path, payload, &req) {
	case C.MESA_GALL_POKE:
		err = n.app.Poke(n, who, b, C.GoString(&req.agent[0]), Noun{req.mark}.Text(), Noun{req.noun})
	case C.MESA_GALL_WATCH, C.MESA_GALL_WATCH_AS:
		err = n.app.Watch(n, who, b, C.GoString(&req.agent[0]), Noun{req.path}.Path())
	case C.MESA_GALL_LEAVE:
		n.app.Leave(n, who, b, C.GoString(&req.agent[0]))
	case C.MESA_GALL_SYSTEM:
	default:
		err = n.app.Plea(n, who, b, Noun{vane}.Text(), Noun{path}, Noun{payload})
	}
	return
}

//export goBoon
func goBoon(ctx C.uintptr_t, her *C.mesa_ship, bone C.uint64_t, payload C.mesa_noun) {
	n := nodeOf(ctx)
	n.hook("boon", func() {
		var mark, noun C.mesa_noun
		switch C.mesa_gall_response(payload, &mark, &noun) {
		case C.MESA_GALL_FACT:
			n.app.Fact(n, shipOf(her), uint64(bone), Noun{mark}.Text(), Noun{noun})
		case C.MESA_GALL_KICK:
			n.app.Kick(n, shipOf(her), uint64(bone))
		default:
			n.app.Boon(n, shipOf(her), uint64(bone), Noun{payload})
		}
	})
}

//export goDone
func goDone(ctx C.uintptr_t, her *C.mesa_ship, bone C.uint64_t, err C.mesa_noun) {
	n := nodeOf(ctx)
	n.hook("done", func() {
		var nack *Nack
		if err != none {
			nack = nackOf(Noun{err})
		}
		n.app.Done(n, shipOf(her), uint64(bone), nack)
	})
}

//export goCorked
func goCorked(ctx C.uintptr_t, her *C.mesa_ship, bone C.uint64_t) {
	n := nodeOf(ctx)
	n.hook("corked", func() { n.app.Corked(n, shipOf(her), uint64(bone)) })
}

//export goSage
func goSage(ctx C.uintptr_t, her *C.mesa_ship, path *C.char, gage C.mesa_noun) {
	n := nodeOf(ctx)
	n.hook("sage", func() {
		p := C.GoString(path)
		if gage == none {
			n.app.Scry(n, shipOf(her), p, "", Noun{}, false)
			return
		}
		g := Noun{gage}
		n.app.Scry(n, shipOf(her), p, g.Head().Text(), g.Tail(), true)
	})
}

//export goPeek
func goPeek(ctx C.uintptr_t, her *C.mesa_ship, path *C.char) (res C.mesa_noun) {
	n := nodeOf(ctx)
	res = none
	n.hook("peek", func() {
		var who *Ship
		if her != nil {
			s := shipOf(her)
			who = &s
		}
		mark, val, ok := n.app.Peek(n, who, C.GoString(path))
		if !ok {
			return
		}
		h, err := build(Cell{mark, val})
		if err != nil {
			logf("mesa: peek %s: %v", C.GoString(path), err)
			return
		}
		res = h
	})
	return
}

//export goBreach
func goBreach(ctx C.uintptr_t, her *C.mesa_ship) {
	n := nodeOf(ctx)
	n.hook("breach", func() { n.app.Breach(n, shipOf(her)) })
}

//  ---------------------------------------------------------------- API

func (n *Node) onLoop() error {
	if t := n.tid.Load(); t == 0 || t != int64(C.mesa_go_thread()) {
		return ErrNotOnLoop
	}
	return nil
}

// pin makes this goroutine the engine's, until unpin.
func (n *Node) pin() {
	runtime.LockOSThread()
	n.tid.Store(int64(C.mesa_go_thread()))
}

func (n *Node) unpin() {
	n.tid.Store(0)
	runtime.UnlockOSThread()
}

func cstr(s string) *C.char { return C.CString(s) }

// OpenFlow opens a new flow to [her] for our requests: one per
// subscription, or one for a series of pokes.
func (n *Node) OpenFlow(her Ship) (uint64, error) {
	if err := n.onLoop(); err != nil {
		return 0, err
	}
	var bone C.uint64_t
	err := check(C.mesa_flow_open(n.eng, her.c(), &bone))
	return uint64(bone), err
}

// Poke sends a poke to [her]'s [agent] on our flow [bone]; App.Done
// says whether it was acked.
func (n *Node) Poke(her Ship, bone uint64, agent, mark string, v Value) error {
	if err := n.onLoop(); err != nil {
		return err
	}
	h, err := build(v)
	if err != nil {
		return err
	}
	a, m := cstr(agent), cstr(mark)
	defer C.free(unsafe.Pointer(a))
	defer C.free(unsafe.Pointer(m))
	return check(C.mesa_gall_poke(n.eng, her.c(), C.uint64_t(bone), a, m, h))
}

// Watch subscribes to [path] on [her]'s [agent], on our flow [bone]
// (a fresh one per subscription). Facts arrive in App.Fact.
func (n *Node) Watch(her Ship, bone uint64, agent, path string) error {
	if err := n.onLoop(); err != nil {
		return err
	}
	h, err := build(Path(path))
	if err != nil {
		return err
	}
	a := cstr(agent)
	defer C.free(unsafe.Pointer(a))
	return check(C.mesa_gall_watch(n.eng, her.c(), C.uint64_t(bone), a, h))
}

// Leave ends our subscription on [bone]; Cork the flow once it's acked.
func (n *Node) Leave(her Ship, bone uint64, agent string) error {
	if err := n.onLoop(); err != nil {
		return err
	}
	a := cstr(agent)
	defer C.free(unsafe.Pointer(a))
	return check(C.mesa_gall_leave(n.eng, her.c(), C.uint64_t(bone), a))
}

// Cork closes our flow [bone] for good, once its messages are done.
func (n *Node) Cork(her Ship, bone uint64) error {
	if err := n.onLoop(); err != nil {
		return err
	}
	return check(C.mesa_flow_cork(n.eng, her.c(), C.uint64_t(bone)))
}

// Fact gives a subscriber on [bone] an update.
func (n *Node) Fact(her Ship, bone uint64, mark string, v Value) error {
	if err := n.onLoop(); err != nil {
		return err
	}
	h, err := build(v)
	if err != nil {
		return err
	}
	m := cstr(mark)
	defer C.free(unsafe.Pointer(m))
	return check(C.mesa_gall_fact(n.eng, her.c(), C.uint64_t(bone), m, h))
}

// Kick ends [her]'s subscription on [bone].
func (n *Node) Kick(her Ship, bone uint64) error {
	if err := n.onLoop(); err != nil {
		return err
	}
	return check(C.mesa_gall_kick(n.eng, her.c(), C.uint64_t(bone)))
}

// Plea sends a raw request [vane path payload] on our flow [bone], for
// vanes other than gall.
func (n *Node) Plea(her Ship, bone uint64, vane, path string, payload Value) error {
	if err := n.onLoop(); err != nil {
		return err
	}
	v, err := build(vane)
	if err != nil {
		return err
	}
	p, _ := build(Path(path))
	pay, err := build(payload)
	if err != nil {
		C.mesa_unref(v)
		C.mesa_unref(p)
		return err
	}
	return check(C.mesa_flow_plea(n.eng, her.c(), C.uint64_t(bone), v, p, pay))
}

// Boon sends a raw response on [her]'s flow [bone].
func (n *Node) Boon(her Ship, bone uint64, payload Value) error {
	if err := n.onLoop(); err != nil {
		return err
	}
	h, err := build(payload)
	if err != nil {
		return err
	}
	return check(C.mesa_flow_boon(n.eng, her.c(), C.uint64_t(bone), h))
}

// Answer finishes a request that App.Plea returned Later for: nil acks
// it, an error nacks it.
func (n *Node) Answer(her Ship, bone uint64, err error) error {
	if e := n.onLoop(); e != nil {
		return e
	}
	h := none
	if err != nil {
		h = errorNoun(err)
	}
	return check(C.mesa_flow_done(n.eng, her.c(), C.uint64_t(bone), h))
}

// Scry reads [path] (e.g. "/g/x/0/agent//some/path") from [her]'s
// public namespace; the answer arrives in App.Scry. Outstanding reads
// are kept in memory only.
func (n *Node) Scry(her Ship, path string) error {
	if err := n.onLoop(); err != nil {
		return err
	}
	p := cstr(path)
	defer C.free(unsafe.Pointer(p))
	return check(C.mesa_keen(n.eng, her.c(), p))
}

// Unscry stops asking for [path]; App.Scry won't be called for it.
func (n *Node) Unscry(her Ship, path string) error {
	if err := n.onLoop(); err != nil {
		return err
	}
	p := cstr(path)
	defer C.free(unsafe.Pointer(p))
	return check(C.mesa_yawn(n.eng, her.c(), p))
}

// Peer is what we know of [her]: keys, rift, sponsor; nil if nothing yet.
func (n *Node) Peer(her Ship) (*Point, error) {
	if err := n.onLoop(); err != nil {
		return nil, err
	}
	var c C.mesa_point
	if !C.mesa_engine_peer(n.eng, her.c(), &c) {
		return nil, nil
	}
	return pointOf(&c), nil
}

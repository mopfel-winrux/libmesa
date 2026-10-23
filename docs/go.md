# Go

`bindings/go` is a cgo binding: package `mesa`, module
`github.com/mopfel-winrux/libmesa/bindings/go`. It links libmesa's static
libraries, so a Go program builds into one binary that needs only libc.
The engine's I/O is Go's own: a `net.UDPConn`, `time.Timer`s, `net/http`
for azimuth and the resolver for galaxy DNS, all driven from one
goroutine. See [host.md](host.md) for what a host does; this binding is
one.

## Building

Build libmesa first (the binding links `zig-out/lib`), then use the
package like any other with cgo enabled:

```
~/zig-x86_64-linux-0.15.2/zig build -Dno-host=true -Doptimize=ReleaseFast
cd bindings/go
go test ./...
go build ./examples/...
```

It needs a C compiler for cgo (gcc or clang) and Go 1.26 or later (what
`modernc.org/sqlite` requires). The
library has no Go dependencies; the tests and examples use
`modernc.org/sqlite`, a database/sql driver in pure Go.

## A ship

```go
import (
	mesa "github.com/mopfel-winrux/libmesa/bindings/go"
	_ "modernc.org/sqlite"
)

type echo struct{ mesa.BaseApp }

func (echo) Poke(n *mesa.Node, her mesa.Ship, bone uint64, agent, mark string, noun mesa.Noun) error {
	if agent != "echo" {
		return mesa.Flub // not an agent we run: they stop asking
	}
	log.Printf("%s says %s", her, noun.Text())
	return nil // ack
}

id, _ := mesa.LoadComet("echo.key")            // mined and saved if new
st, _ := mesa.OpenSQLite("sqlite", "echo.db")
n, _ := mesa.Open(mesa.Config{Identity: id, Store: st, App: echo{}})
n.Run(ctx)
```

**Identities.** `LoadKeyfile(path)` or `ParseKeyfile(text)` for planets,
stars, galaxies and moons (what `urbit -k` takes, or what `|moon`
prints); `LoadComet(path)` mines a comet and saves its keyfile at `path`,
owner-only, or loads the one already there; `FakeIdentity(ship)` for
fake ships (`urbit -F`).

**Config.** `Identity`, `Store` and `App` (nil: drop everything).
`Port` 0 binds a galaxy's own port, or an ephemeral one. `Breached`
allows an empty store for an identity that has run before: only after a
breach (see the README's [State and identity](../README.md#state-and-identity)).
Comets and fake ships never need it. `Trace` (or `$MESA_TRACE`) logs
every datagram. `Helpers` see requests and responses before the App
([Helpers](#helpers)). `Open` binds the socket, checks our keys against
azimuth for galaxies, stars and planets, opens the engine, and starts
the helpers.

**Run** drives the node until its context is done or `Close` is called.
`Close` stops it and closes the engine and socket; close the store
yourself after.

## Handlers

An `App` has a method per thing that can happen; embed `mesa.BaseApp`
and override what you need. They run on the node's goroutine, inside the
event's store transaction.

| method | when | return |
| --- | --- | --- |
| `Poke(n, her, bone, agent, mark, noun)` | a poke to our agent | `nil` acks, `mesa.Flub` for agents we don't run (or `mesa.Drop`), any other error nacks |
| `Watch(n, her, bone, agent, path)` | a subscription | same; give facts with `n.Fact` on `bone` |
| `Leave(n, her, bone, agent)` | they unsubscribed | |
| `Plea(n, her, bone, vane, path, payload)` | any other request | as Poke, or `mesa.Later` and answer with `n.Answer` |
| `Fact(n, her, bone, mark, noun)` | an update on our subscription | |
| `Kick(n, her, bone)` | our subscription was ended | |
| `Boon(n, her, bone, payload)` | any other response | |
| `Done(n, her, bone, nack)` | our message was acked (`nack` nil) or nacked | |
| `Corked(n, her, bone)` | our flow is closed for good | |
| `Scry(n, her, path, mark, noun, ok)` | the answer to `n.Scry` (`ok` false: they breached) | |
| `Peek(n, her, path)` | a read of our namespace (`her` nil if public or `%shut`) | `mark, value, true`, or `ok` false |
| `Breach(n, her)` | they breached; our flows with them are gone | |

A nack's error goes back to the peer as `[%go-error tang]` with the
error's text; return a `*mesa.Nack{Tag, Lines}` to choose the tag and
lines. A panic in a handler is logged, and in Poke, Watch or Plea nacks.

`BaseApp` flubs every poke and watch (it runs no agents) and drops
every other plea.

Flub, drop and nack differ. A dropped request gets no answer at all, so
the peer resends it later. A flubbed one gets none either, and the flow
halts: the sender's gall is told to stop until `n.Spur(agent)` says the
agent is back, then resends; Arvo's gall does this for agents that
aren't running. From `Plea` (other vanes), `Flub` is `Drop`. All three
undo the handler's writes, which needs a store with savepoints (both
stores here have them; see [store.md](store.md#savepoints)).

An App that also implements `ShutKeyer`,
`ShutKey(n, her, agent, spur string) uint64`, hands out `%shut` keys:
`her`'s gall asks for the key to `agent`'s paths at `spur` (`/secret/x`,
after `//1`); return a kid from `n.ShutAdd`, or 0 to refuse. Apps that
don't implement it refuse everyone.

## Sending

The node's methods call the engine, so they work only on its goroutine:
in a handler, or in a function given to `Do`. Anywhere else they return
`mesa.ErrNotOnLoop`.

```go
err := n.Do(func() error {
	bone, err := n.OpenFlow(her)
	if err != nil {
		return err
	}
	return n.Poke(her, bone, "chat", "noun", "hello")
})
```

`Do` runs its function in one transaction and returns its error: if it
fails, its writes and messages are undone. Called before `Run`, it runs
right away (a `Run` starting meanwhile waits for it). `n.After(d, fn)`
runs `fn` on the loop, in a transaction, once `d` has passed.

| method | |
| --- | --- |
| `OpenFlow(her)` | a new flow for our requests: one per subscription, or one for a series of pokes |
| `Poke(her, bone, agent, mark, value)` | poke; `Done` says how it went |
| `Watch(her, bone, agent, path)` / `Leave(her, bone, agent)` | subscribe on a fresh flow; facts arrive in `Fact` |
| `Cork(her, bone)` | close our flow, after a leave is acked |
| `Fact(her, bone, mark, value)` / `Kick(her, bone)` | to a subscriber of ours |
| `Scry(her, path)` / `Unscry(her, path)` | remote scry, e.g. `/g/x/1/agent//1/path` |
| `ShutAdd(prefix, key)` | serve the paths under `prefix` only encrypted with `key` (32 random bytes); returns its kid |
| `RequestKey(her, bone, path)` | ask their gall for the key to a `%shut` path; the answer is a `Boon` on `bone`: `mesa.ParseKey(payload)` gives `kid, key, ok` |
| `ScryShut(her, kid, key, path)` | read a `%shut` path with its key; the answer arrives in `Scry` |
| `Spur(agent)` | our agent is back: what we flubbed for it is resent |
| `Plea`, `Boon`, `Answer` | raw requests and responses, for other vanes |
| `Peer(her)` | the keys, rift and sponsor we have for them |

Messages on one flow are delivered in order, one at a time; see
[urbit-apps.md](urbit-apps.md) for gall's protocol, `%shut` and building
nouns.

Outstanding scries (`Scry`, `ScryShut`) live in memory only: after a
restart no `Scry` answer comes for them, so ask again; keep what you
asked for in your own table if you need to.

## Helpers

Helpers carry protocol code that every app would otherwise write into
its handlers. Pass them in `Config.Helpers`; each sees requests and
responses before the App, and the first to claim one gets it (the App
doesn't). A breach goes to every helper, then to the App. They run on
the loop, inside the event's transaction, like handlers.

Their state lives in the node's `Store` (`n.Store()`) beside the
engine's, under keys starting with `a` (the engine's are upper case), so
it commits with each ack and survives restarts, with either store. The
key format is the Python binding's too: one database works with both. A
node has at most one `Subs`; `Pokes` are told apart by `Name`.

**Subs** keeps subscriptions alive:

```go
subs := &mesa.Subs{
	OnFact:  func(n *mesa.Node, s mesa.Sub, mark string, noun mesa.Noun) { ... },
	OnWatch: func(n *mesa.Node, s mesa.Sub, err *mesa.Nack) { ... }, // optional
	OnKick:  func(n *mesa.Node, s mesa.Sub) { ... },                  // optional
}
n, _ := mesa.Open(mesa.Config{..., Helpers: []mesa.Helper{subs}})
n.Do(func() error { return subs.Subscribe(n, her, "my-agent", "/updates") })
```

- `Subscribe(n, her, agent, path)` watches on a fresh flow, unless that
  subscription exists already; call it on the loop.
- Facts on its flows go to `OnFact` with the `Sub` (`Ship`, `Agent`,
  `Path`), not to `App.Fact`.
- `OnWatch`: the watch was acked (`err` nil), or nacked: the
  subscription is over and isn't retried.
- A kick corks the old flow (as gall does) and resubscribes on a new
  one: right away the first time, then after 1, 2, 4... seconds, at most
  `Backoff` (default 5 minutes), until a subscription stays up a minute.
  The publisher's breach resubscribes the same way. Pending resubscribes
  are in the store, so they happen after a restart too.
- `Unsubscribe(n, her, agent, path)` leaves and corks; `List()` is the
  subscriptions.

**Pokes** sends pokes on one flow per `(ship, agent)`, opened on the
first and remembered, so they arrive in the order sent:

```go
pokes := &mesa.Pokes{OnDone: func(n *mesa.Node, her mesa.Ship, agent string, seq uint64, err *mesa.Nack) { ... }}
seq, err := pokes.Poke(n, her, "my-agent", "noun", "hi") // on the loop
```

`Poke` numbers pokes from 1 per `(ship, agent)`; `OnDone` reports each
ack or nack, in order. A breach nacks every unanswered poke with tag
`breach` and forgets the flow. `Bone(her, agent)` is the flow.

**Tlon DMs**, package `github.com/mopfel-winrux/libmesa/bindings/go/tlon`:

```go
var dms tlon.DMs
dms.OnMessage = func(n *mesa.Node, m tlon.Message) error { // m.From, .Text, .ID, .Sent, .Story
	_, err := dms.Send(n, m.From, "you said: "+m.Text)
	return err
}
dms.OnSent = func(n *mesa.Node, her mesa.Ship, id string, err *mesa.Nack) {} // optional
n, _ := mesa.Open(mesa.Config{..., Helpers: []mesa.Helper{&dms}})
```

It runs as agent `%chat`: it answers Tlon's version check with `%chat-dms`
version 2, gives each new DM to `OnMessage` once (in the transaction that
acks it; an error nacks it; a resent message with the same author and
sent time is acked without calling it again), and leaves other agents to
the App. `Send` (or `SendStory`) DMs on one flow per ship and returns the
message's id, its sent `@da` in decimal; `OnSent` reports delivery.
`tlon.Story(text)` and `tlon.StoryText(noun)` convert between text and
Tlon stories. See [urbit-apps.md](urbit-apps.md#worked-example-tlon-dms)
for the protocol.

**Writing one**: embed `mesa.BaseHelper` and override `Start` and the
`Handle*` methods you need (`HandlePoke` and `HandleWatch` return
`(claimed, err)`, the others `claimed`). Keep state in `n.Store()` under
keys starting with `a` and a letter no other helper uses; use `n.After`
for timers.

## Nouns

Nouns going out are Go values:

| Go | noun |
| --- | --- |
| `nil` | `~` |
| `bool` | a loobean (`true` is 0) |
| integers, `*big.Int`, `[]byte` (little-endian) | an atom |
| `string` | a cord (`@t`) |
| `mesa.Ship` | an `@p` |
| `mesa.Tuple(a, b, c)` | `[a b c]` |
| `mesa.List(a, b)`, `mesa.Path("/a/b")`, `mesa.Tape(s)`, `mesa.Some(v)` | `~[a b]`, `/a/b`, a tape, `[~ v]` |
| `mesa.Da(t)` | an `@da` |
| `mesa.Jammed(bytes)` | a jammed noun |
| a `mesa.Noun` | itself |

Nouns coming in are `mesa.Noun` handles, **borrowed for the call**:
`IsCell`, `Head`, `Tail`, `At(axis)`, `Uint64`, `Bytes`, `Big`, `Text`,
`List`, `Path`, `Tape`, `String` (for logs). To keep one past the call,
copy out what you need, or `Jam()` it and pass it back later as
`mesa.Jammed`. `mesa.Time(noun)` reads an `@da`, `mesa.Tang(noun)` an error
noun.

## Stores

`OpenSQLite(driver, path)` keeps the ship's state in SQLite through any
database/sql driver (`"sqlite"` for `modernc.org/sqlite`, `"sqlite3"` for
`mattn/go-sqlite3`; import it yourself), in the same table and with the
same lock file as libmesa's C store and the Python binding's, so one
database works with all three. It holds one connection, `Conn()`: put
your own tables there, and write them from handlers or `Do`, and they
commit in the same transaction as the ack. That makes processing
exactly-once: a crash before the commit loses both your change and the
ack, and the peer resends.

`NewMemoryStore()` keeps everything in memory (tests, throwaway ships).
Any type with the `Store` methods works; implement `Savepoints` too if
your handlers ever nack or drop ([store.md](store.md)).

## Examples

- `examples/ping`: poke an agent and print the ack or nack (`-c -s
  comet.db ~zod ping hi`), or stay online printing pokes (`-l`).
- `examples/dmbot`: a Tlon DM bot on package `tlon`. DM it from Tlon and
  it answers; `help` lists what it knows. It sends each reply in the
  same transaction that acks the message it answers, so a crash never
  loses a reply or sends one twice.

The tests (`go test -race ./...`) run fake galaxies 201-218 on
127.0.0.1.

## Notes

- The engine is pinned to the OS thread `Run` runs on
  (`runtime.LockOSThread`); handlers run there too. Engines don't share
  state, so several nodes can run in one process, each on its own
  goroutine.
- Blocking work never runs on the loop: azimuth lookups and galaxy DNS
  happen on goroutines and post their results back. A datagram to a
  galaxy whose address isn't known yet is dropped, and the engine resends
  it.
- `Open` can block for an azimuth lookup of our own point (galaxies,
  stars and planets), and `LoadComet` downloads the star list and mines.
- Our own moons are out of reach on the live network: the engine asks
  the host for their keys, and azimuth doesn't have them.

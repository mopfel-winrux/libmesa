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

It needs a C compiler for cgo (gcc or clang) and Go 1.24 or later. The
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
		return mesa.Drop // not an agent we run: no answer
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
every datagram. `Open` binds the socket, checks our keys against
azimuth for galaxies, stars and planets, and opens the engine.

**Run** drives the node until its context is done or `Close` is called.
`Close` stops it and closes the engine and socket; close the store
yourself after.

## Handlers

An `App` has a method per thing that can happen; embed `mesa.BaseApp`
and override what you need. They run on the node's goroutine, inside the
event's store transaction.

| method | when | return |
| --- | --- | --- |
| `Poke(n, her, bone, agent, mark, noun)` | a poke to our agent | `nil` acks, `mesa.Drop` for agents we don't run, any other error nacks |
| `Watch(n, her, bone, agent, path)` | a subscription | same; give facts with `n.Fact` on `bone` |
| `Leave(n, her, bone, agent)` | they unsubscribed | |
| `Plea(n, her, bone, vane, path, payload)` | any other request | as Poke, or `mesa.Later` and answer with `n.Answer` |
| `Fact(n, her, bone, mark, noun)` | an update on our subscription | |
| `Kick(n, her, bone)` | our subscription was ended | |
| `Boon(n, her, bone, payload)` | any other response | |
| `Done(n, her, bone, nack)` | our message was acked (`nack` nil) or nacked | |
| `Corked(n, her, bone)` | our flow is closed for good | |
| `Scry(n, her, path, mark, noun, ok)` | the answer to `n.Scry` (`ok` false: they breached) | |
| `Peek(n, her, path)` | a read of our namespace (`her` nil if public) | `mark, value, true`, or `ok` false |
| `Breach(n, her)` | they breached; our flows with them are gone | |

A nack's error goes back to the peer as `[%go-error tang]` with the
error's text; return a `*mesa.Nack{Tag, Lines}` to choose the tag and
lines. A panic in a handler is logged, and in Poke, Watch or Plea nacks.

Drop and nack differ: a dropped request gets no answer at all, so the
peer resends it later; Arvo does the same for agents that aren't running.
Both undo the handler's writes, which needs a store with savepoints
(both stores here have them; see [store.md](store.md#savepoints)).

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
right away.

| method | |
| --- | --- |
| `OpenFlow(her)` | a new flow for our requests: one per subscription, or one for a series of pokes |
| `Poke(her, bone, agent, mark, value)` | poke; `Done` says how it went |
| `Watch(her, bone, agent, path)` / `Leave(her, bone, agent)` | subscribe on a fresh flow; facts arrive in `Fact` |
| `Cork(her, bone)` | close our flow, after a leave is acked |
| `Fact(her, bone, mark, value)` / `Kick(her, bone)` | to a subscriber of ours |
| `Scry(her, path)` / `Unscry(her, path)` | remote scry, e.g. `/g/x/1/agent//1/path` |
| `Plea`, `Boon`, `Answer` | raw requests and responses, for other vanes |
| `Peer(her)` | the keys, rift and sponsor we have for them |

Messages on one flow are delivered in order, one at a time; see
[urbit-apps.md](urbit-apps.md) for gall's protocol and building nouns.

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
- `examples/dmbot`: a Tlon DM bot. DM it from Tlon and it answers;
  `help` lists what it knows. It sends each reply in the same transaction
  that acks the message it answers, so a crash never loses a reply or
  sends one twice.

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

# Python

`bindings/python/libmesa.py` drives libmesa from Python with ctypes. The
engine in `libmesa.so` does the protocol; Python does the rest of what
libmesa-host does in C: the UDP socket and select loop, timers, galaxy
DNS, azimuth lookups, and the store (a SQLite database written by Python's
`sqlite3`, which the application can keep its own tables in).

See also: [getting started](getting-started.md), [writing a host](host.md),
[stores](store.md), [talking to Urbit apps](urbit-apps.md).

## Requirements

- `libmesa.so`, built with zig 0.15.2:

  ```
  ~/zig-x86_64-linux-0.15.2/zig build                     # everything
  ~/zig-x86_64-linux-0.15.2/zig build -Dno-host=true      # skip libmesa-host
  ```

  Either makes `zig-out/lib/libmesa.so`: the core with its crypto and
  SQLite inside. The binding doesn't need libmesa-host (libuv, curl,
  OpenSSL), so `-Dno-host=true` is enough.

- Python 3 on a Unix (the store locks with `fcntl.flock`). Standard
  library only: `ctypes`, `sqlite3`, `socket`, `select`, `urllib`. The
  example's `http.server.ThreadingHTTPServer` needs 3.7 or later.

The module loads the library at import, trying in order:

1. `$LIBMESA`, a path to the `.so`;
2. `../../zig-out/lib/libmesa.so` relative to `libmesa.py` (the repo's
   build output);
3. `libmesa.so` through the dynamic linker's search path.

If none loads, `import libmesa` raises `OSError`. The module isn't
packaged; put `bindings/python` on `sys.path` (the example does
`sys.path.insert` with its parent directory).

## Ships

```python
import libmesa as m

zod = m.ship("~zod")              # Ship; ValueError if not a ship name
str(zod)                          # "~zod" (same as m.ship_text(zod))
m.Ship(lo, hi)                    # from the two 64-bit halves of the @p
```

`Ship` is the C `mesa_ship` struct; it compares and hashes by value, so
it works as a dict key.

## Identities

`Identity` is who the node is. Three constructors:

| | |
| --- | --- |
| `Identity.fake("~nec")` | fake keys derived from the name, as `urbit -F`; also accepts a `Ship` |
| `Identity.keyfile(path)` | a planet, star, galaxy or moon keyfile (`@uw` text, as `urbit -k` takes) |
| `Identity.comet(path)` | the comet whose keyfile is at `path`; if there's no file, mines one and writes it there |

`Identity.comet` fetches `https://bootstrap.urbit.org/comet-stars.jam`,
mines a comet under one of those stars (this takes a while; it prints
`mining a comet under N stars...` to stderr), and saves the keyfile at
`path` with `mesa_keyfile_render()`, readable only by you (mode 0600;
it won't overwrite an existing file). Later runs read the file back. Keep
it with its database: the comet's state is in the database, its keys are
in the file. `tlon_dms.py -c -s dms.db` keeps them at `dms.db.key`.

`keyfile()` reads the first identity in the file. `identity.ship` is the
`Ship`; `identity.fake` is true for fake identities.

## The store

`SqliteStore(path)` is the engine's `mesa_store`, written in Python. A
`Node` makes one from its `db_path`; applications don't usually build it
themselves.

- It takes an exclusive, non-blocking `flock` on `<path>.lock` and raises
  `MesaError("... is in use by another process")` if another process
  holds it. The lock is held until `node.close()`.
- It opens `path` with `sqlite3` in autocommit mode
  (`isolation_level=None`), sets `journal_mode=WAL` and
  `synchronous=FULL`, and keeps libmesa's state in one table,
  `mesa_kv(k BLOB PRIMARY KEY, v BLOB NOT NULL)`. That is the schema and
  lock file of the C store (`mesa_store_sqlite`), so the same database
  works with either.
- The engine's transactions are `BEGIN IMMEDIATE` / `COMMIT` /
  `ROLLBACK`; its savepoints are `SAVEPOINT m<n>` / `ROLLBACK TO` /
  `RELEASE`, nested by depth. Savepoints are what roll back a handler's
  writes when it nacks or drops.
- An exception inside a store callback is printed and returned to the
  engine as an error (-1).

The connection is `store.db`, and `node.db` is the same object. Your
tables go there: anything a handler writes through `self.db` is in the
event's transaction and commits with the ack, or is rolled back with it.
Outside a handler, wrap writes in `with node.txn():` to commit them
together with messages you send; otherwise each statement autocommits on
its own.

Other threads may read the database through their own connections (WAL
allows concurrent readers). Don't write through `node.db` from another
thread; see [Threads](#threads).

## Node

```python
Node(identity, db_path, breached=False, port=0, fake=None, helpers=())
```

- `identity`: an `Identity`.
- `db_path`: the SQLite database (created if missing; locked; see above).
- `breached`: allow an empty database for an identity that isn't a comet
  or fake: it has never run before, or its rift went up since it last
  ran. Without it, opening fresh state raises `MesaError("this identity
  has run before, but its state is empty: ...")`. Comets and fake ships
  start fresh on their own (the node sets `breached` for fake ones). See the README's
  [State and identity](../README.md#state-and-identity).
- `port`: the UDP port to bind (0: any). A fixed port keeps the address
  other ships know us by across restarts. Galaxies default to 13337+n
  (31337+n when fake).
- `fake`: a fake network (`urbit -F`); defaults to `identity.fake`.
- `helpers`: `Helper`s that see requests and responses before your
  handlers ([Helpers](#helpers)).

The constructor, in order:

1. opens the store and binds the socket (`0.0.0.0`, or `127.0.0.1` when
   fake);
2. calls `self.setup()`, for creating your tables;
3. for stars and planets (not fake), looks the identity up on azimuth
   (blocking), raises `MesaError` if it isn't there or its keys or life
   don't match, and takes its rift (if the keyfile didn't give one) and
   sponsor from there;
4. opens the engine (`mesa_engine_open`). Other errors raise
   `MesaError("mesa_engine_open: error N")`, with `N` a `mesa_err` from
   `mesa/engine.h` (-3 is a database that belongs to another ship, life
   or rift);
5. starts each helper (`helper.start(node)`), which loads its state.

Attributes: `node.our` (our `Ship`), `node.db`, `node.store`, `node.eng`
(the `mesa_engine*`), `node.sock`.

### The loop

`node.run()` loops on `node.step()` until `node.stop()` or Ctrl-C, then
calls `node.close()`, which closes the engine, the database connection
and the socket, and releases the lock.

`node.step(timeout=0.1)` is one turn:

1. `select()` on the socket and `self.extra_fds()`, for at most
   `timeout` seconds or until the engine's next deadline;
2. reads up to 256 datagrams into `mesa_engine_hear`;
3. calls `self.on_fd(fd)` for each extra fd that is readable;
4. hands the engine any keys the resolver threads found
   (`mesa_engine_point`, or `mesa_engine_point_missing`);
5. runs functions posted with `call_soon()` (galaxy lookups post theirs
   here) and timers set with `after()` that are due, each in its own
   transaction;
6. runs the engine's due timers (`mesa_engine_wake`);
7. calls `self.on_tick()`.

So `on_tick` runs at least every 100 ms. An exception from `on_tick` or
`on_fd` is printed and the loop goes on. Exceptions in the other
handlers are printed and handled as below.

You can call `step()` yourself from your own loop instead of `run()`.

### Handlers

Subclass `Node` and override what you need. Every handler below except
`on_tick` and `on_fd` runs inside the engine's transaction for the event
that caused it.

| method | when | return |
| --- | --- | --- |
| `setup(self)` | once, in the constructor, before the engine opens | |
| `on_poke(self, her, agent, mark, noun)` | a gall poke to `agent` | ack, nack or `DROP` (below) |
| `on_watch(self, her, bone, agent, path)` | a gall subscription (`%s`, or `%l` whose mark is ignored) | ack (accept), nack (refuse) or `DROP` |
| `on_leave(self, her, bone, agent)` | a gall leave; always acked | ignored |
| `on_plea(self, her, bone, vane, path, payload)` | any plea that isn't a gall request | ack, nack, `LATER` or `DROP` |
| `on_fact(self, her, bone, mark, noun)` | a fact on a subscription of ours | ignored |
| `on_kick(self, her, bone)` | our subscription on `bone` was kicked (`[%x ~]`) | ignored |
| `on_boon(self, her, bone, payload)` | any other response on a flow of ours | ignored |
| `on_done(self, her, bone, error)` | our plea on `bone` was acked (`error` is `None`) or nacked (`error` is a list of lines) | ignored |
| `on_corked(self, her, bone)` | our cork of `bone` was acked: the flow is gone (no `on_done` for the cork) | ignored |
| `on_scry(self, her, path, mark, noun)` | the answer to `scry()`; `mark` and `noun` are `None` if it was dropped (`her` breached) | ignored |
| `on_peek(self, her, path)` | a read of our namespace; `her` is `None` for public reads | `(mark, value)` or `None` |
| `on_breach(self, her)` | `her` breached; every flow with them is gone | ignored |
| `on_tick(self)` | every loop turn, outside any transaction | ignored |
| `extra_fds(self)` | each turn: more objects for `select()` to wait on | list |
| `on_fd(self, fd)` | one of those is readable, outside any transaction | ignored |

Argument types: `her` is a `Ship`; `bone` an int; `agent`, `mark` and
`path` are `str` (paths as `"/a/b"`); `noun`, `vane`, `payload` are
borrowed `Noun` handles. The defaults drop every poke, watch and plea,
serve nothing, and ignore everything else.

How the return value of a request handler becomes an answer:

- `ACK`, `NACK`, `LATER` and `DROP` mean what they say. Any other value
  acks if truthy and nacks if falsy, so `True`/`False` work, and `None`
  (falling off the end) nacks. `LATER` is meant for `on_plea`.
- If any of the three raises, the traceback is printed and the plea is
  nacked with the error `[%python-error [%leaf "<message>"] ~]` (the
  first 200 characters of the exception).
- A nack or drop rolls back the handler's writes (a savepoint); an ack
  commits them with the ack. A drop sends nothing: the peer keeps
  resending, as Arvo does for an agent that isn't running.
- `LATER` means "I'll answer this later": call `node.done(her, bone)`
  to ack, or `node.done(her, bone, error)` to nack, from a transaction
  of your own. Later messages on that flow wait until then.

Gall's own system flows (`/gf`, `/gk`) and jael's are answered by the
engine and never reach the handlers.

A watch's ack comes back to the subscriber as its watch-ack. To send
facts, call `self.fact(her, bone, mark, value)` with the `bone` from
`on_watch`, either inside `on_watch` (as `tlon_dms.py` does for Tlon's
version check) or later inside `with node.txn():`. `self.kick(her,
bone)` ends their subscription.

`on_peek` serves remote scries of our namespace. `path` is the inner
path, such as `/g/x/0/my-agent//some/path`. Return `(mark, value)`,
which becomes the `[mark noun]` cell, or `None` for nothing there. As in
C, bindings must be immutable: once a path has a value, it must always
have the same value.

### Sending

```python
bone = node.open_flow(her)                       # a new forward flow; its bone
node.poke(her, bone, agent, mark, value)         # value: anything noun() takes
node.watch(her, bone, agent, "/some/path")
node.leave(her, bone, agent)
node.cork(her, bone)                             # close the flow once its pleas are done
node.fact(her, bone, mark, value)                # on their backward flow (a subscription to us)
node.kick(her, bone)                             # end their subscription
node.scry(her, "/g/x/0/agent//some/path")        # answer arrives in on_scry
node.unscry(her, "/g/x/0/agent//some/path")      # stop asking; no on_scry
```

For vanes other than gall: `node.plea(her, bone, vane, path, payload)`
sends a raw plea on our flow, `node.boon(her, bone, payload)` a raw boon
on theirs, and `node.done(her, bone, error=None)` answers a `LATER`.
`node.peer(her)` returns the `Point` (keys, rift, sponsor) the engine
has for `her`, or `None`.

All raise `MesaError("mesa_...: error N")` on failure (see `mesa_err` in
`mesa/engine.h`). The values you pass are converted with `noun()` and
transferred to the engine.

Messages on one flow arrive in order, one at a time, and `on_done` for
a flow comes in the order its pleas were sent. Use one flow for a series
of pokes and a flow of its own per subscription; after a leave is acked,
cork that flow, as gall does. Bones are integers you can store in your
tables (`tlon_dms.py` keeps one per conversation).

Call these from a handler, or anywhere else inside `with node.txn():`,
so they commit with your writes. `txn()` begins and commits an engine
transaction (`mesa_txn_begin`/`mesa_txn_commit`); an exception inside the
block aborts it (`mesa_txn_abort`) and propagates. Inside a handler
`txn()` does nothing, since the handler is already in one. Outside any
transaction each engine call runs in its own, which is fine for a single
send.

Datagrams go out only after the transaction commits. Nothing is sent
until the loop runs. Outstanding scries live in memory only: ask again
after a restart.

### Keys, galaxies, tracing

When the engine needs a peer's keys (`hooks.need_point`), the node
starts a daemon thread that calls azimuth's `getPoint` at
`https://roller.urbit.org/v1/azimuth` (`MESA_AZIMUTH_URL`) with urllib,
30 s timeout, one thread per ship at a time. The thread puts the result
on a queue, and the loop hands it to the engine in `step()`; a failed or
empty lookup is reported as missing. On a fake network, keys come from
the ship's name, without a thread or a request. Moons' keys come from
their parents through the engine, not azimuth, except our own moons',
which the node can't resolve on the live network
([host.md](host.md#moons)).

A datagram for a galaxy goes to `<galaxy>.urbit.org` port 13337+n,
looked up with `socket.gethostbyname` and cached for five minutes. The
lookup runs on a daemon thread, one per galaxy at a time, and posts its
answer back to the loop, which never waits for it: until it's in, a
datagram for that galaxy goes to the last address known (a failed lookup
keeps it), or nowhere, and the engine resends. On a fake network galaxies are `127.0.0.1:31337+n`, and every datagram goes
to 127.0.0.1 whatever address it names.

Set `MESA_TRACE` to any non-empty value to print one line to stderr per
datagram sent and heard: its size and address. Unlike mesa-ping's trace,
it doesn't decode packets.

## Nouns

Going in, nouns are plain Python values. `noun(x)` converts:

| Python | noun |
| --- | --- |
| `None` | `~` (0) |
| `int` (non-negative) | atom |
| `str` | cord (UTF-8 bytes, little-endian) |
| `bytes`, `bytearray` | atom from little-endian bytes |
| `True` / `False` | loobean: `%.y` (0) / `%.n` (1) |
| tuple or list of 2+ items | cells, right-associated: `(a, b, c)` is `[a b c]` |
| `Noun` | the same noun, with a new reference |

So `("add", 5, None)` is `[%add 5 ~]`. A Hoon list needs its `None`
terminator: `(1, 2, 3, None)`. Since `Noun` converts, you can put a
noun you received into one you send while the handler runs.

Helpers that build values for `noun()`:

- `path("/a/b")` is `("a", "b", None)`; `path("/")` is `None`.
- `da(unix=None)` is an `@da` (int) from Unix seconds, default now;
  `unix(d)` goes back.

`noun()` returns a raw handle (an int) that you own. The `Node` methods
take Python values and transfer the result for you, so you rarely call
it yourself; if you do, release it with `m.lib.mesa_unref(n)` or pass it
to a call that takes ownership.

Coming out, handlers get `Noun` objects:

- `n.is_cell`, `n.head`, `n.tail`;
- `int(n)`, `n.bytes()` (little-endian), `n.text()` (an atom as UTF-8);
  `int()` raises `TypeError` on a cell;
- `n.py()`: the whole noun as ints and `(head, tail)` pairs;
- `cord(n)`: the text of an atom, or `None` for a cell (also takes a raw
  handle);
- `tang(n)`: the lines of an error noun `[tag tang]`, best effort (`leaf`,
  `rose` and `palm` tanks; others render as `""`). `on_done` already does this,
  so its `error` is a list of `str`.

A `Noun` is borrowed: it is only valid while the handler that received it
runs. Copy out what you need (`int`, `.text()`, `.py()`) before
returning; don't keep `Noun`s in `self`.

## Threads

The engine is single-threaded, and so is the node: its loop, every
handler, and `node.db` belong to the thread that calls `run()`. The only
other threads are the azimuth and galaxy-DNS lookups, which talk to the
loop through queues.

To do something from another thread (a web server, a REPL), don't call
`poke()` or write `node.db` there. Hand the loop a function with
`node.call_soon(fn)`: it runs on the loop's next turn, in a transaction.
Or put the work on your own queue and drain it in `on_tick`, inside a
transaction:

```python
def setup(self):
    self.todo = queue.Queue()

def on_tick(self):
    while not self.todo.empty():
        her, text = self.todo.get()
        with self.txn():
            bone = self.open_flow(her)
            self.poke(her, bone, "chat", "noun", text)
```

`on_tick` runs at least every 100 ms, so the latency is that. For a
lower one, add a pipe or socketpair to `extra_fds()`, write a byte to it
from the other thread, and drain the queue in `on_fd`.

## Helpers

Helpers carry the protocol code that every app would otherwise write in
its handlers: keeping subscriptions alive, ordering pokes, Tlon's DM
protocol. Pass them to the node, `Node(..., helpers=[subs, pokes])`;
each sees requests and responses before your `on_*` methods, and the
first to claim one gets it (yours don't). A breach goes to every helper,
then to `on_breach`. Like handlers, they run on the loop inside the
event's transaction.

Their state lives in `mesa_kv` beside the engine's, under keys starting
with `a` (the engine's are upper case), so it commits with each ack and
survives restarts. The key format is the Go binding's too: one database
works with either. A node has at most one `Subs`; `Pokes` are told
apart by `name`.

### Subs

```python
subs = m.Subs(on_fact=lambda node, sub, mark, noun: print(sub.path, noun.py()),
              on_watch=lambda node, sub, error: print(sub, "refused" if error else "ok"),
              on_kick=None, backoff=300)
node = m.Node(ident, "sub.db", helpers=[subs])
with node.txn():
    subs.subscribe(node, m.ship("~sampel-palnet"), "my-agent", "/updates")
node.run()
```

- `subscribe(node, her, agent, path)` watches on a fresh flow, unless
  that subscription exists already. Call it in a handler or `with
  node.txn():`.
- Facts on its flows go to `on_fact(node, sub, mark, noun)`; a `Sub` has
  `ship`, `agent`, `path`.
- `on_watch(node, sub, error)`: the watch was acked (`error` None), or
  nacked: the subscription is over and isn't retried.
- A kick corks the old flow (as gall does) and resubscribes on a new
  one: right away the first time, then after 1, 2, 4... seconds, at most
  `backoff`, until a subscription stays up a minute. `on_kick(node, sub)`
  is told. The publisher's breach resubscribes the same way.
- `unsubscribe(node, her, agent, path)` leaves and corks. `list()` is the
  subscriptions.

Backoff timers are kept in the database, so a resubscribe due when the
node stopped happens when it starts again.

### Pokes

```python
pokes = m.Pokes(on_done=lambda node, her, agent, seq, error: ..., name="")
with node.txn():
    seq = pokes.poke(node, her, "my-agent", "noun", "hi")
```

One flow per `(ship, agent)`, opened on the first poke and remembered,
so pokes arrive in the order sent. `poke()` returns a sequence number,
from 1 per `(ship, agent)`; `on_done(node, her, agent, seq, error)`
reports each ack (`error` None) or nack (its lines), in order. A breach
nacks every unanswered poke with `["breach"]` and forgets the flow.
`bone(her, agent)` is the flow, if any.

### Writing one

Subclass `m.Helper` and override what you need: `start(node)`;
`handle_poke(node, her, bone, agent, mark, noun)` and `handle_watch(node,
her, bone, agent, path)` return `None` (not ours) or an answer, as
`on_poke`'s; `handle_fact`, `handle_kick`, `handle_done`,
`handle_corked` return True to claim; `handle_breach`. For state,
`node.kv_get(k)`, `kv_put(k, v)`, `kv_del(k)` and `kv_scan(prefix)`
work on `mesa_kv` in the current transaction (pick keys starting with
`a` and a letter no other helper uses); `node.after(seconds, fn)` runs
`fn()` later on the loop, in a transaction.

## Tlon DMs: tlon.py

`bindings/python/tlon.py` is a helper for Tlon's DM protocol
(tloncorp/tlon-apps, desk/app/chat.hoon):

```python
import libmesa as m, tlon

def message(node, msg):                   # msg.ship, .text, .id, .sent, .story
    dms.send(node, msg.ship, "you said: " + msg.text)

dms = tlon.DMs(on_message=message,
               on_sent=lambda node, her, id_, error: None)
node = m.Node(m.Identity.comet("bot.key"), "bot.db", helpers=[dms])
node.run()
```

- It runs as agent `%chat`: it answers Tlon's version check (a watch on
  `/~/negotiate/version/chat-dms`) with a `noun` fact of `2`, acks
  `%chat` pokes that aren't DM diffs and DM deltas other than `%add`
  (deletes, reactions, replies), and leaves other agents to you.
- `on_message(node, msg)` gets each new message once, in the transaction
  that acks it; raise to nack it (its writes are undone). Tlon resends a
  message on a new flow sometimes; one with the same author and `sent`
  time is acked without calling `on_message` again.
- `send(node, her, text)` pokes `chat-dm-diff-2` with
  `[[author sent] %add [[story author sent] [%chat ~] ~ ~] ~]` on one
  flow per ship (through `Pokes`), and returns the message's id: its
  `sent` `@da`, in decimal, fresh per message. `send_story()` takes a
  story noun. `on_sent(node, her, id, error)` reports delivery.
- `story(text)` builds a story (one paragraph, line breaks as `%break`);
  `story_text(noun)` flattens one to text (formatting dropped, links and
  ship mentions as text, blocks as `[kind]`).

## Tests

```
python3 -m unittest bindings/python/test_bindings.py
```

Fake galaxies 220-226 on 127.0.0.1, driven from the test's own thread
with `step()`: subscriptions across kicks and a restart, ordered pokes,
a DM round trip, and galaxy lookups off the loop.

## Examples

### An agent that prints pokes

```python
import sys
sys.path.insert(0, "/path/to/libmesa/bindings/python")
import libmesa as m

class Echo(m.Node):
    def on_poke(self, her, agent, mark, noun):
        if agent != "echo":
            return m.DROP                 # not ours: no answer, they resend
        print(her, "poked %echo with", mark, m.cord(noun) or noun.py())
        return True                       # ack

    def on_peek(self, her, path):
        if path == "/g/x/0/echo//hello":
            return ("noun", "hello")
        return None

node = Echo(m.Identity.comet("echo.key"), "echo.db")
print(node.our, "is online")
node.run()
```

From a dojo: `:~comet-name/echo 'hi'`. Returning `m.DROP` for other
agents leaves their pleas unanswered, so a later app that runs them on
the same identity still gets them.

### Poking ~zod's %ping

```python
import libmesa as m

class Client(m.Node):
    def on_done(self, her, bone, error):
        print("ack" if error is None else "nack: " + "\n".join(error))
        self.stop()

node = Client(m.Identity.comet("client.key"), "client.db")
zod = m.ship("~zod")
with node.txn():
    bone = node.open_flow(zod)
    node.poke(zod, bone, "ping", "noun", "hi")
node.run()
```

The poke is stored with the transaction and sent once the loop runs;
`~zod`'s keys are looked up on azimuth in the meantime. Poking `%hood`
instead gets a nack, whose `error` is `~zod`'s stack trace. This is what
`mesa-ping -c -s comet.db ~zod ping hi` does in C.

### Subscribing

```python
class Sub(m.Node):
    def on_done(self, her, bone, error):
        print("watch-ack" if error is None else "watch-nack")

    def on_fact(self, her, bone, mark, noun):
        print(mark, noun.py())

node = Sub(m.Identity.comet("sub.key"), "sub.db")
her = m.ship("~sampel-palnet")
with node.txn():
    bone = node.open_flow(her)            # one flow per subscription
    node.watch(her, bone, "my-agent", "/updates")
node.run()
```

To end it: `node.leave(her, bone, "my-agent")`, then `node.cork(her,
bone)` once the leave is acked (`on_corked` follows when the cork is).
A kick arrives in `on_kick`. `Subs` ([Helpers](#subs)) does all of this,
and resubscribes after kicks.

## tlon_dms.py

`bindings/python/examples/tlon_dms.py` is a complete application: your
ship runs in Python, Tlon users DM it, and you read and answer at a web
page.

```
python3 bindings/python/examples/tlon_dms.py -k ~sampel-palnet.key -s dms.db --web 127.0.0.1:8474
python3 bindings/python/examples/tlon_dms.py -c -s dms.db --web 8474     # as a comet, keys in dms.db.key
python3 bindings/python/examples/tlon_dms.py -F ~nec -s nec.db           # a fake ship
```

Options: `-k KEYFILE`, `-c` or `-F SHIP` (one of them); `-s DB`
(required); `--fresh` (passed as `breached`); `--web [ADDR:]PORT`
(default `127.0.0.1:8474`); `--port` (the UDP port).

It is in three parts.

**The ship.** `DMs(m.Node)` passes a `tlon.DMs` helper
([Tlon DMs](#tlon-dms-tlonpy)) to the node, which speaks the protocol;
the class keeps the conversations for the page:

- `setup()` creates `dm` (one row per conversation: ship, last activity,
  unread count; its `bone` column is from before `tlon.DMs`) and
  `dm_msg` (each message, with a unique index on `(ship, mine, sent)`),
  and the `todo` queue.
- `got()`, the helper's `on_message`, stores the message and counts it
  unread, in the transaction that acks it.
- `send()` sends with `dms.send()` and stores our message as `sending`,
  under the id it returns; `sent()`, the helper's `on_sent`, marks it
  `sent` or `failed`.
- `on_done` marks acks on the flow it used before `tlon.DMs` (so a
  database from then keeps working).
- `on_tick` drains `todo`, running each job (`("send", ship, text)`,
  `("read", ship)`) in `with self.txn():`.

**The web page.** `serve_web()` runs a `ThreadingHTTPServer` in a daemon
thread with its own read-only connection to the database
(`file:...?mode=ro`). `GET /state?after=N` returns the conversations and
new messages (and our recent ones again, for status changes);
`POST /send`, `/read` put jobs on `node.todo`; `POST /check` validates a
ship name. The page polls every 1.5 s. The web thread never touches the
engine or `node.db`.

**main()** picks the identity (`Identity.keyfile`, `Identity.comet(DB +
".key")` or `Identity.fake`), builds `DMs(ident, db, breached=--fresh,
port=--port)`, starts the web server and calls `node.run()`.

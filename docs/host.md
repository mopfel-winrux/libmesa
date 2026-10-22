# Writing your own host

The engine (`mesa/engine.h`) does no I/O. A *host* owns the UDP socket,
the clock and the key resolver, feeds the engine what happens, and carries
out what the engine asks for. libmesa ships two hosts:

- `src/host/host.c` (libmesa-host): libuv, libcurl, `uv_getaddrinfo`.
- `bindings/python/libmesa.py` (`Node`): Python sockets, `select`,
  `urllib` and threads, over ctypes ([python.md](python.md)).

Write your own to use another event loop or another language. This page
is everything a host has to do, checked against those two and the engine
sources (`src/eng/`). See [getting-started.md](getting-started.md) for
identities and a first program, [store.md](store.md) for the store
interface, and [urbit-apps.md](urbit-apps.md) for the application hooks.

## The contract

| the host | how |
| --- | --- |
| sends datagrams | implements `hooks.send` |
| receives datagrams | calls `mesa_engine_hear` |
| keeps time | calls `mesa_engine_wake` at `mesa_engine_deadline` |
| finds peers' keys | answers `hooks.need_point` with `mesa_engine_point` or `mesa_engine_point_missing` |
| knows where galaxies are | resolves `MESA_LANE_GALAXY` lanes |
| checks our own keys | looks up our point before `mesa_engine_open`, fills `our_sponsor` |
| runs everything on one thread | the engine and nouns are not thread-safe |

The application's hooks (`plea`, `boon`, `done`, `corked`, `sage`,
`peek`, `breach`) are the application's business; a host passes them
through. libmesa-host keeps the application's `hooks.ctx` by installing
trampolines that take the host as `ctx` and call the application's hook
with the application's `ctx` (`_tr_plea` and friends in `host.c`).

## Opening the engine

Fill a `mesa_config`:

- `ident`: from `mesa_keyfile_parse()`, `mesa_comet_mine()` or
  `mesa_ident_fake()`. `ident.ring[0]` must be `'B'`, or
  `mesa_engine_open` returns `MESA_E_ARG`. A rift of `MESA_RIFT_UNKNOWN`
  is taken as 0 by the engine; resolve it first (below).
- `store`: any `mesa_store` ([store.md](store.md)).
- `hooks`: yours, with `send` and `need_point` set.
- `our_sponsor`: see [Our own point](#our-own-point).
- `breached`: allow an empty store. The engine only exempts comets; fake
  ships need it on every run. `tools/cli.c` sets it for `-F`, and `Node`
  passes `breached or self.fake`.
- `no_ping`: leave false unless you don't want the 25s `/app/ping` to our
  galaxy (galaxies never ping).
- `fake`: true on a fake network (`urbit -F`); see [Fake ships](#fake-ships).
- `now`: the current time in milliseconds since the Unix epoch.

`mesa_engine_open` returns `MESA_E_FRESH` for an empty store without
`breached`, `MESA_E_IDENTITY` for a store that belongs to another ship or
to a later life or rift, `MESA_E_STORE` if the store fails (a `get` or
`scan` error included).

**Have the socket and the hooks ready before calling `mesa_engine_open`.**
Opening restarts whatever was in flight, in its own transaction, so it can
call `hooks.send` (resending queued messages) and `hooks.need_point`
(looking up a sponsor chain to route them) before it returns. Both hosts
bind their socket first; libmesa-host starts reading it only after the
engine is open.

## Time

All times are milliseconds since the Unix epoch, as `uint64_t`: the `now`
arguments of `mesa_engine_hear` and `mesa_engine_wake`, `mesa_config.now`,
and the value `mesa_engine_deadline` returns. The engine only uses
differences between them (retransmission, the 30s key retry, the 25s
ping, liveness), and its clock never goes backwards: a `now` smaller than
the last one is ignored.

The engine's clock advances only on `mesa_engine_hear` and
`mesa_engine_wake`. API calls (`mesa_flow_*`, `mesa_keen`, `mesa_gall_*`)
and `mesa_engine_point` use the last time it saw.

The timer loop:

```c
uint64_t dea = mesa_engine_deadline(eng);   //  absolute ms
if ( UINT64_MAX == dea ) {
  //  nothing scheduled: wait for I/O only
}
else if ( dea <= now_ms() ) {
  mesa_engine_wake(eng, now_ms());
}
else {
  //  wait at most dea - now_ms()
}
```

`mesa_engine_deadline` is cheap (no store access) and must be read again
after *every* call into the engine: any input can create or move a
deadline. `mesa_engine_wake` runs a full store transaction each time, so
don't call it in a busy loop; call it when the deadline has passed. It is
safe to call early.

libmesa-host does this in `mesa_host_kick()`: read the deadline, stop the
timer on `UINT64_MAX`, else start a one-shot timer for `dea - now` (0 if
already due), whose callback calls `mesa_engine_wake` and kicks again.
`Node.step()` computes the `select` timeout from the deadline on each
turn (capped at 0.1s) and calls `mesa_engine_wake` if it has passed.

## Datagrams

### hooks.send

```c
void (*send)(void* ctx, mesa_lane lane, const uint8_t* buf, size_t len);
```

- It is called synchronously, at the end of the engine call that produced
  the datagrams, after the store transaction commits. Datagrams from a
  transaction that rolls back or fails to commit are discarded and never
  reach `send`. Inside `mesa_txn_begin`/`mesa_txn_commit`, they're sent
  from `mesa_txn_commit`; from a hook, when the event that ran the hook
  commits.
- `buf` is valid only for the duration of the call; the engine frees it
  when `send` returns. Copy it if you send asynchronously.
- Datagrams are at most 1472 bytes (`PACT_SIZE`).
- Never block. Send non-blocking and drop on `EAGAIN` or any error: the
  engine retransmits. libmesa-host uses `uv_udp_try_send`; `Node` catches
  `OSError`.
- Don't call back into the engine from `send`.

### mesa_engine_hear

```c
void mesa_engine_hear(mesa_engine* eng, mesa_lane from,
                      const uint8_t* buf, size_t len, uint64_t now);
```

Pass every datagram received on the socket, with `from` set to the
sender's address as a `MESA_LANE_IP` lane. The engine copies what it
needs; `buf` is yours again when it returns. Datagrams longer than 1472
bytes are dropped, so a 2048-byte receive buffer is plenty (both hosts
use one).

Don't filter. The engine tells mesa packets from classic ames packets
itself (`mesa_is_new_pact()`): classic packets go to
`src/eng/eng_ames.c`, which answers a ship whose keys it has with an
`%ahoy` to move it to mesa, and asks `hooks.need_point` for one it
doesn't know (the ship resends). Garbage is dropped.

`hear` runs the application's hooks for whatever the datagram completes,
and calls `send` before returning. Re-arm the timer afterwards.

## Lanes

```c
typedef struct mesa_lane {
  mesa_lane_kind kind;      //  MESA_LANE_NONE, _GALAXY, _IP
  uint8_t        galaxy;
  uint32_t       ip;        //  IPv4, host byte order
  uint16_t       port;      //  host byte order
} mesa_lane;
```

- `MESA_LANE_IP`: `ip` and `port` are host byte order. 1.2.3.4:5678 is
  `ip = 0x01020304`, `port = 5678`; in C, `ntohl(sin_addr.s_addr)` and
  `ntohs(sin_port)` on receive, `htonl`/`htons` on send. In Python,
  `int.from_bytes(socket.inet_aton(ip), "big")`. Both hosts drop IP
  lanes with `ip == 0` or `port == 0` (libmesa-host also
  `0xffffffff`). IPv4 only; IPv6 isn't supported yet.
- `MESA_LANE_GALAXY`: "galaxy `galaxy`'s address". The engine routes to
  galaxies, and through a peer's galaxy when it has no direct route, with
  this lane; the host resolves it:
  - live network: the A record of `<name>.urbit.org`, where `<name>` is
    the galaxy's `@p` without the `~` (`mesa_ship_render()` of
    `{ .lo = galaxy }`, skipping the first character), at UDP port
    `13337 + galaxy`. `zod.urbit.org:13337`, `nec.urbit.org:13338`.
  - fake network: `127.0.0.1:31337 + galaxy`.

  libmesa-host resolves asynchronously (`uv_getaddrinfo`, IPv4), caches
  each address for 5 minutes, and drops the datagram while a lookup is
  outstanding ("the engine will retry once we know"). The domain is
  `mesa_host_config.dns_domain`, default `urbit.org`; libmesa-host does
  not call azimuth's `getDns`. `mesa_azimuth_dns_request()` and
  `mesa_azimuth_dns_parse()` exist for a host that wants the domain list
  from azimuth: POST the request to the azimuth URL like `getPoint`
  (below); the result is the list of domains, `"urbit.org"` for the live
  network. `Node._galaxy()` hard-codes `urbit.org` and resolves with a
  blocking `gethostbyname()` inside `send`, caching for 300s; a new host
  should resolve off the loop as libmesa-host does.
- `MESA_LANE_NONE`: never reaches `send` (the engine drops it), but ignore
  unknown kinds anyway.

On a fake network both hosts also send every IP lane to `127.0.0.1`,
keeping the port, and bind the socket to `127.0.0.1`.

### Our port

Galaxies must be reachable at their well-known port: bind `13337 + g`
(`31337 + g` on a fake network) for galaxy `g`. Everyone else binds an
ephemeral port (0); the engine learns peers' return addresses from what
it hears. libmesa-host binds `INADDR_ANY` (127.0.0.1 when fake), or
`mesa_host_config.port` if set.

## Keys

### hooks.need_point

```c
void (*need_point)(void* ctx, mesa_ship who);
```

The engine needs `who`'s keys, rift and sponsor. Answer later, from the
loop, with exactly one of:

```c
void mesa_engine_point(mesa_engine* eng, const mesa_point* pot);
void mesa_engine_point_missing(mesa_engine* eng, mesa_ship who);
```

**Don't call either from inside `need_point`.** The hook runs in the
middle of an engine operation (sending, routing, hearing); answering
there re-enters the engine while it's iterating its state. Queue the
ship and answer after the current engine call returns. libmesa-host
pushes it on a list and drains it from a `uv_idle` callback; `Node`
answers through a `queue.Queue` drained in `step()`. That holds even when
the answer is immediate (fake ships).

`mesa_engine_point` copies `*pot` (the pointer is borrowed) and runs its
own transaction: new keys start whatever was waiting for them (`send` is
called before it returns); a rift above the one we had is a breach
(`hooks.breach`, all flows with that ship are gone); a new life or key is
a rekey. A point with `life == 0` or `pass[0] != 'b'` records only the
sponsor. Points for our own ship are ignored. Re-arm the timer after it.

`mesa_engine_point_missing` means "not now": it touches no store and
sends nothing, and only restarts that peer's 30-second retry clock.

When the engine asks:

- for a peer it has messages, requests or a classic packet for but no
  keys: then again every 30s (the deadline includes it) until answered;
- for a star or planet on some peer's sponsorship chain whose sponsor it
  doesn't know, so escaped ships route to the right galaxy;
- every hour (`ENG_POINT_MS`) for known non-comet peers with requests
  outstanding, to catch rekeys and breaches.

Asks for one ship are at most one per 30s, but a slow answer can overlap
the next ask; `Node` keeps a set of ships being resolved to skip
duplicates. Don't cache answers for long: breaches and rekeys reach the
engine through these answers.

### Azimuth

For galaxies, stars and planets, the point comes from the azimuth-rpc
JSON-RPC API that the Urbit roller serves:

```
POST https://roller.urbit.org/v1/azimuth       (MESA_AZIMUTH_URL)
Content-Type: application/json

{"jsonrpc":"2.0","id":"~sampel-palnet","method":"getPoint","params":{"ship":"~sampel-palnet"}}
```

`mesa_azimuth_point_request(who)` builds that body (free it with
`mesa_free()`); `mesa_azimuth_point_parse(who, len, json, &pot)` reads the
response:

- `MESA_AZIMUTH_OK`: `pot` is filled; pass it to `mesa_engine_point`.
- `MESA_AZIMUTH_NOT_FOUND`: not spawned (RPC error -32000).
- `MESA_AZIMUTH_BAD`: anything else.

Both hosts answer `mesa_engine_point_missing` on anything but OK,
including HTTP and network errors. libmesa-host uses a 30s timeout and
looks for the system CA bundle itself (or `$MESA_CA_FILE`), because its
bundled curl doesn't know the system's trust store.

`mesa_azimuth_fetch(url, who, &pot)` in libmesa-host is the blocking
version of all this (with `url` 0 for the default); a C host linking
libmesa-host can call it from a worker thread instead of writing its own
HTTP client.

### Comets

The engine never calls `need_point` for a comet. It fetches the comet's
attestation (`/pawn/proof`) from the comet itself and checks it with
`mesa_comet_check()`: the key's fingerprint must be the comet's address,
and its sponsor (`+sein`) a star. A host has nothing to do. (libmesa-host
still answers comets with `point_missing`; that branch is never taken.)

### Moons

A moon isn't on azimuth. On the real network, for someone else's moon
the engine asks the moon's parent itself, with a jael `/public-keys` plea
on a flow it keeps open, and never calls `need_point` for it
(`eng_sys_moon()` in `src/eng/eng_sys.c`).

Two exceptions call `need_point` for a moon:

- **our own moons** (we are the moon's parent): the engine assumes the
  application knows their keys. A host that runs a ship with moons must
  answer these with a `mesa_point` it builds (the moon's rift, life and
  `pass`, sponsor = us). Both libmesa-host and `Node` answer
  `point_missing` (libmesa-host directly; `Node` after azimuth says not
  found), so neither can talk to its own moons on the live network.
- **`cfg.fake`**: every ship's keys come from its name, moons' included,
  as in fake Arvo.

### Fake ships

On a fake network, answer every `need_point` without I/O:

```c
mesa_ident ide;
mesa_point pot = { .who = who, .rift = 0, .life = 1,
                   .has_sponsor = true, .sponsor = mesa_ship_sein(who) };
mesa_ident_fake(who, &ide);
mesa_ident_pass(&ide, pot.pass);
//  later, from the loop:
mesa_engine_point(eng, &pot);
```

(`Node` sets `has_sponsor = False` instead; the engine then falls back
to `+sein`, which is the same.) Set `mesa_config.fake`, `breached`, and
route galaxies to `127.0.0.1:31337 + g`.

### Our own point

Before opening the engine, libmesa-host (`mesa_host_open()`) checks our
own point, unless we're fake, a comet or a moon:

1. Fetch our `getPoint`. Failing that (including not spawned):
   `MESA_HOST_E_AZIMUTH`.
2. Compare azimuth's `life` and `pass` with `ident.life` and
   `mesa_ident_pass(&ident, pass)`. Mismatch: `MESA_HOST_E_KEYS` (the
   keyfile is stale or wrong; peers couldn't talk to us).
3. If `ident.rift == MESA_RIFT_UNKNOWN`, take azimuth's rift.
4. If `has_sponsor`, set `cfg.our_sponsor` to the sponsor.

`our_sponsor` matters for stars and planets: it's where the engine routes
our pings and where peers reach us through. The engine ignores it for
galaxies, moons (the parent) and comets (`+sein`), and falls back to
`+sein` when it's `{0, 0}`. That is also `~zod`, so `~zod` can't be named
explicitly: a star that escaped to `~zod` gets its `+sein` instead. `Node` does the same check, but only for stars
and planets, and then sets an unknown rift to 0.

## Threading

The engine is single-threaded: make every call to it, and to the noun
functions, from one thread, and run all hooks on it (they're called from
inside engine calls). Blocking work (HTTP, DNS) goes on other threads,
and its results come back to the loop thread as messages:

- libmesa-host: `uv_queue_work` runs `mesa_azimuth_fetch` on libuv's
  thread pool; its after-work callback runs on the loop thread and calls
  `mesa_engine_point` or `mesa_engine_point_missing`, then
  `mesa_host_kick`. `examples/guestbook.c` posts webhooks the same way.
- `Node`: a daemon thread per lookup puts `(ship, Point or None)` on a
  `queue.Queue`; `step()` drains it after reading the socket.

The worker side may call functions that touch neither the engine nor
nouns: `mesa_azimuth_point_request`/`parse` (as `Node`'s threads do),
`mesa_ship_render`, `mesa_free`. Build nouns and `mesa_point`-to-engine
calls on the loop thread. A loop that blocks in `poll`/`select` needs a
way to be woken by a worker: a pipe, `eventfd`, or a short timeout (`Node`
uses at most 0.1s).

## Calling the engine from outside a hook

The application calls the flow API (`mesa_flow_open`, `mesa_gall_poke`,
`mesa_keen`, ...) whenever it likes, on the loop thread. Each call runs as
its own transaction, unless the application has one open:

```c
mesa_txn_begin(eng);            //  MESA_E_STATE inside a hook
//  ...application writes in the same store...
mesa_gall_poke(eng, her, bone, "agent", "noun", mesa_atom_cord("hi"));
mesa_txn_commit(eng);           //  datagrams go to hooks.send here
//  or mesa_txn_abort(eng): rolls back, drops the datagrams, reloads
```

Afterwards, **re-arm the timer**: the call may have created a deadline
(a retransmission) the loop doesn't know about. That's all
`mesa_host_kick()` is for. `Node` gets it for free by recomputing the
timeout every turn, provided the call is made from the loop thread
(`on_tick`, `on_fd`, or a hook).

If the loop was idle for a while, the engine's clock is stale; the
resulting deadline is then already due and the next wake fixes it.

### From inside hooks

- `plea`, `boon`, `done`, `corked`, `breach`, `sage`, `peek`: the flow
  API may be called (they run inside the event's transaction, so the
  writes and sends commit with it). `mesa_txn_begin` returns
  `MESA_E_STATE` from `plea`, `boon`, `done`, `corked` and `breach`;
  don't call it from any hook. A `plea` handler that nacks or drops has
  its API calls rolled back with its writes; that needs a store with
  savepoints ([store.md](store.md#savepoints)).
- `send`, `need_point`: call nothing on the engine.

## Engine failure

If the store fails, the engine logs `mesa: store failed; engine stopped`
and stops: inputs are ignored and API calls return `MESA_E_FAILED`
(`mesa_txn_commit` too). Its memory no longer matches the store. Close it
and open a new engine from the same store; see
[store.md](store.md#failure).

Shut down with `mesa_engine_close()` (it rolls back an open transaction
and frees the engine, not the store), then `mesa_store_free()`.

Logging goes to stderr; `mesa_set_log()` (`mesa/log.h`) redirects it,
process-wide, before any engine is created. `$MESA_TRACE` makes the engine
log every packet it sends and hears; both hosts also log each datagram's
address with it.

## Memory across an FFI

`zig build` makes `libmesa.so` for other languages: the core, with its
crypto and SQLite inside, and none of libmesa-host. Everything below is
in that library.

**Buffers.** Anything libmesa returns for you to free comes from its own
allocator; free it with `mesa_free()`, never your language's `free`:

| returns a buffer you free | |
| --- | --- |
| `mesa_ship_render` | `@p` text |
| `mesa_keyfile_render` | keyfile text |
| `mesa_azimuth_point_request`, `mesa_azimuth_dns_request` | JSON bodies |
| `mesa_azimuth_dns_parse` | each string written to `out` |
| `mesa_atom_get_cord`, `mesa_jam_bytes` | |

Anything you hand libmesa to free must come from `mesa_malloc()`: the
value a store's `get` returns. Buffers libmesa passes *to* you are
borrowed for the call: `send`'s `buf`, `sage`'s and `peek`'s `path`, a
store `scan` callback's key and value.

**Structs** are passed by value: `mesa_ship` (two `uint64_t`, `lo` first),
`mesa_lane` (`int` kind, `uint8_t`, `uint32_t`, `uint16_t`, C padding),
and by pointer: `mesa_ident`, `mesa_point` (`pass[65]` then a C `bool`),
`mesa_config`, `mesa_hooks`, `mesa_store`. Mirror the layouts exactly;
`libmesa.py` has them as ctypes `Structure`s. If your FFI can't take
structs by value in callbacks, write a small C shim.

**Nouns** are `uint64_t` handles: small atoms are the value itself
(shifted), larger atoms and cells are tagged pointers to refcounted
boxes. To a garbage-collected language they're plain integers; nothing
of them needs pinning. `MESA_NONE` (`UINT64_MAX`) means "no noun".

- Constructors (`mesa_atom_*`, `mesa_cell`, `mesa_trel`, `mesa_qual`,
  `mesa_cue`, `mesa_cue_bytes`) return a reference you own. `mesa_cell`
  and friends *transfer* their arguments.
- Accessors (`mesa_head`, `mesa_tail`, `mesa_at`) return borrowed nouns,
  valid while the parent lives. `mesa_ref()` one to keep it.
- Engine and gall API calls (`mesa_flow_plea`, `mesa_flow_boon`,
  `mesa_flow_done`, `mesa_gall_poke`/`watch`/`fact`) transfer their nouns,
  on success and on error alike.
- Nouns passed to hooks are borrowed for the call. Outputs of
  `mesa_gall_response` and `mesa_gall_request_parse` are borrowed from
  their input.
- What a hook gives back is transferred to the engine: `peek`'s return
  value and `plea`'s `*error`.

So a non-C host must `mesa_unref()` every noun it built and didn't hand
to the engine (e.g. the result of `mesa_cue_bytes` once read), and must
not keep a borrowed handle past its hook without `mesa_ref()`. Never
unref a borrowed noun. `mesa_unref(MESA_NONE)` is a no-op;
`mesa_ref(MESA_NONE)` aborts.

**Callbacks** must stay alive as long as the engine (or store) that holds
them: ctypes `CFUNCTYPE` objects are kept on the `Node` (`self._hooks`,
`self.store.c`) for that reason. `hooks.ctx` is an opaque `void*`;
pass a handle (an index, `cgo.Handle`) rather than a pointer into
garbage-collected memory. `Node` passes `None` and uses bound methods
instead.

**Exceptions** must not cross into C. `Node` wraps every hook
(`_guard`): an exception in a `plea` handler becomes a nack with a
`[%python-error tang]` error, anywhere else it's logged and ignored, and a
store callback that raises returns -1 (which stops the engine). Its loop
also logs exceptions from `on_tick` and `on_fd` and carries on.

## A host in C, without libuv

A sketch: one UDP socket, `poll`, resolver threads that write results to
a pipe. Error handling and galaxy DNS caching omitted.

```c
#include "mesa/engine.h"
#include <arpa/inet.h>
#include <poll.h>
#include <sys/time.h>

typedef struct host {
  mesa_engine* eng;
  int          sock;
  int          pip[2];    //  resolver threads -> loop
  bool         fake;
  //  galaxy addresses, a queue of ships to resolve, ...
} host;

static uint64_t
now_ms(void)
{
  struct timeval tv;
  gettimeofday(&tv, 0);
  return (uint64_t)tv.tv_sec * 1000 + tv.tv_usec / 1000;
}

static void
h_send(void* ctx, mesa_lane lan, const uint8_t* buf, size_t len)
{
  host*              h = ctx;
  struct sockaddr_in to = { .sin_family = AF_INET };

  if ( MESA_LANE_GALAXY == lan.kind ) {
    uint32_t ip;
    if ( h->fake ) {
      to.sin_addr.s_addr = htonl(0x7f000001);
      to.sin_port = htons(31337 + lan.galaxy);
    }
    else if ( galaxy_cached(h, lan.galaxy, &ip) ) {   //  else: start a lookup
      to.sin_addr.s_addr = htonl(ip);                 //  off-thread and drop
      to.sin_port = htons(13337 + lan.galaxy);
    }
    else return;
  }
  else if ( MESA_LANE_IP == lan.kind && lan.ip && lan.port ) {
    to.sin_addr.s_addr = htonl(h->fake ? 0x7f000001 : lan.ip);
    to.sin_port = htons(lan.port);
  }
  else return;

  sendto(h->sock, buf, len, MSG_DONTWAIT, (struct sockaddr*)&to, sizeof(to));
}

static void
h_need_point(void* ctx, mesa_ship who)
{
  enqueue(ctx, who);      //  answered from the loop, never from here
}

/* resolver thread: getPoint, then hand the result to the loop
*/
static void*
resolve(void* arg)
{
  struct job { host* h; mesa_ship who; } *j = arg;
  struct { mesa_ship who; bool ok; mesa_point pot; } res = { .who = j->who };
  char*  req = mesa_azimuth_point_request(j->who);
  size_t len;
  char*  rsp = http_post(MESA_AZIMUTH_URL, "application/json", req, &len);

  res.ok = rsp && (MESA_AZIMUTH_OK ==
                   mesa_azimuth_point_parse(j->who, len, rsp, &res.pot));
  write(j->h->pip[1], &res, sizeof(res));   //  < PIPE_BUF: atomic
  mesa_free(req);
  free(rsp);
  free(j);
  return 0;
}

static void
drain_needs(host* h)
{
  mesa_ship who;
  while ( dequeue(h, &who) ) {
    if ( h->fake ) {
      mesa_ident ide;
      mesa_point pot = { .who = who, .life = 1, .has_sponsor = true,
                         .sponsor = mesa_ship_sein(who) };
      mesa_ident_fake(who, &ide);
      mesa_ident_pass(&ide, pot.pass);
      mesa_engine_point(h->eng, &pot);
    }
    else if ( MESA_EARL == mesa_ship_rank(who) ) {
      mesa_engine_point_missing(h->eng, who);  //  or our own moon's point
    }
    else {
      start_thread(resolve, job(h, who));     //  skip if already in flight
    }
  }
}

int
run(host* h)
{
  for ( ;; ) {
    struct pollfd pfd[2] = { { h->sock, POLLIN }, { h->pip[0], POLLIN } };
    uint64_t      dea = mesa_engine_deadline(h->eng);
    uint64_t      now = now_ms();
    int           tim = ( UINT64_MAX == dea ) ? -1
                      : ( dea <= now ) ? 0
                      : (int)(dea - now > 60000 ? 60000 : dea - now);

    poll(pfd, 2, tim);

    if ( pfd[0].revents & POLLIN ) {
      uint8_t            buf[2048];
      struct sockaddr_in fro;
      socklen_t          fen = sizeof(fro);
      ssize_t            n;

      while ( 0 < (n = recvfrom(h->sock, buf, sizeof(buf), MSG_DONTWAIT,
                                (struct sockaddr*)&fro, &fen)) )
      {
        mesa_lane lan = { .kind = MESA_LANE_IP,
                          .ip   = ntohl(fro.sin_addr.s_addr),
                          .port = ntohs(fro.sin_port) };
        mesa_engine_hear(h->eng, lan, buf, (size_t)n, now_ms());
        fen = sizeof(fro);
      }
    }

    if ( pfd[1].revents & POLLIN ) {
      struct { mesa_ship who; bool ok; mesa_point pot; } res;
      while ( sizeof(res) == read(h->pip[0], &res, sizeof(res)) ) {  //  O_NONBLOCK
        if ( res.ok ) mesa_engine_point(h->eng, &res.pot);
        else          mesa_engine_point_missing(h->eng, res.who);
      }
    }

    drain_needs(h);       //  need_point asks made by the calls above

    if ( mesa_engine_deadline(h->eng) <= now_ms() ) {
      mesa_engine_wake(h->eng, now_ms());
      drain_needs(h);
    }

    app_tick(h);          //  the application's own work: flow API calls
  }                       //  here are picked up by the next deadline read
}
```

Setup, in order: create and bind the socket (well-known port for a
galaxy), check our own point, fill `mesa_config` with `hooks.ctx = h`,
`h_send`, `h_need_point` and the application's hooks, then
`mesa_engine_open(&cfg, &h->eng)`. Drain needs once after opening, since
`mesa_engine_open` may already have asked.

## Notes for FFI hosts

`bindings/python/libmesa.py` is a complete host in about 1000 lines and the
best template for another language:

- **Declarations.** It declares every struct (`Ship`, `Ident`, `Point`,
  `Lane`, `Store`, `Hooks`, `Config`, `GallRequest`) and every function
  signature it uses (`_sig()`), including `restype`; ctypes would
  otherwise truncate `uint64_t` returns to `int`.
- **Store.** `SqliteStore` implements `mesa_store` in Python over
  `sqlite3`, so the application's tables share libmesa's transactions.
  `get` copies the value into `mesa_malloc()` memory. A Go host can do the
  same over its own SQLite driver, or use `mesa_store_sqlite()` from the
  library and `mesa_store_sqlite_db()` for its tables (only usable from C,
  since it returns a `sqlite3*`). See [store.md](store.md).
- **Identity.** `Identity.fake`, `.keyfile`, `.comet` (downloads
  `bootstrap.urbit.org/comet-stars.jam`, `mesa_cue_bytes`, mines with
  `mesa_comet_mine`, saves with `mesa_keyfile_render`).
- **Startup.** `Node.__init__` binds the socket, runs `setup()` for the
  application's tables, checks our own point for stars and planets, then
  builds `Config` and calls `mesa_engine_open`, mapping `-2`
  (`MESA_E_FRESH`) to an explanation.
- **Loop.** `step()`: compute the timeout from `mesa_engine_deadline`;
  `select` on the socket and `extra_fds()`; read up to 256 datagrams into
  `mesa_engine_hear`; hand other ready fds to `on_fd`; drain resolver
  results into `mesa_engine_point`/`_missing`; wake if due; `on_tick()`.
- **Hooks.** Each hook converts its arguments (`Ship` copied out of the
  by-value struct, nouns wrapped as borrowed `Noun` handles valid only
  during the call) and calls an `on_*` method. Plea and boon payloads are
  decoded with `mesa_gall_request_parse` and `mesa_gall_response` before
  the application sees them.
- **Transactions.** `node.txn()` is `mesa_txn_begin`/`commit`/`abort`
  as a context manager, and a no-op inside a hook (`in_hook`), where the
  event's transaction is already open.

For Go specifically:

- Own the engine from one goroutine and call `runtime.LockOSThread()` in
  it: the header asks for a single thread, and cgo callbacks then arrive
  on that same thread. Deliver resolver and application requests to it
  over channels; wake its `poll` with a pipe or `eventfd`, or keep
  the socket in a separate reader goroutine that forwards datagrams on a
  channel and drive the engine from a `select` over the channels and a
  `time.Timer` reset from `mesa_engine_deadline` after each call.
- Export the hooks with `//export` and point the `mesa_hooks` fields at
  them from C; pass a `cgo.Handle` as `ctx`.
- Copy `send`'s buffer (`C.GoBytes`) if it goes to another goroutine.
- Nouns are `C.uint64_t`; wrap owned ones in a type with an explicit
  `Free()` (finalizers would run on another goroutine, against the
  single-thread rule).

## libmesa-host's own order of operations

For reference, `mesa_host_open()`: global curl init; check our point;
initialize the UDP handle, the timer and the idle handle, and bind the
socket; open the engine with its own `send`, `need_point` and the
application's hooks behind trampolines; start receiving; then
`mesa_host_kick()`. So a datagram the engine sends while resuming
in-flight flows at open goes out, and a `need_point` during open is
queued as usual.

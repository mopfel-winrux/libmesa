# libmesa

A standalone C library for speaking Urbit's **mesa** networking protocol, so
any application can talk to Urbit ships as if it were one: poke and
subscribe to gall agents, remote-scry their namespaces, and be poked and
scried in turn.

Mesa is split across two codebases: vere's `io/mesa.c` is only the
transport, while crypto, flows, acks and namespace serving live in Arvo's
`ames.hoon` (the `|mesa` core). libmesa extracts the first and ports the
second to C, as one sans-IO engine.

Status: working against real Arvo. A libmesa fake galaxy pokes, watches,
remote-scries and is poked by a fake `~zod` running stock vere and Arvo
(see [Interop test](#interop-test)), with state persisted in SQLite across
restarts.

On the live network, a freshly mined libmesa comet pokes `~zod` directly
(`%ping` acks; `%hood` nacks with its stack trace), with its state persisted
across runs.

## Using it

```c
#include "mesa/host.h"
#include "mesa/gall.h"

mesa_ident ide;                       //  from mesa_keyfile_parse(),
mesa_keyfile_parse(text, &ide, 1);    //  mesa_comet_mine() or mesa_ident_fake()

const char* err;
mesa_host_config cfg = {
  .engine = {
    .ident = ide,
    .store = mesa_store_sqlite("ship.db", &err),
    .hooks = { .ctx = app, .done = on_done, .boon = on_boon,
               .plea = on_plea, .sage = on_sage, .peek = on_peek },
  },
};
mesa_host* hos;
mesa_host_open(&cfg, &hos);

mesa_engine* eng = mesa_host_engine(hos);
uint64_t bone;
mesa_flow_open(eng, her, &bone);
mesa_gall_poke(eng, her, bone, "my-agent", "noun", mesa_atom_cord("hi"));
mesa_host_run(hos);
```

`tools/mesa-ping.c` is a complete example: `mesa-ping -c ~sampel-palnet
my-agent hello` mines a comet and pokes an agent; `-w /path` watches,
`-x /g/x/1/agent//1/path` remote-scries. Set `MESA_TRACE=1` to log every
datagram libmesa-host sends and hears.

```
$ mesa-ping -c -s comet.db ~zod ping hi
mining a comet under 5 stars...
we are ~waltus-fonser-bospen-larmex--nommet-donryt-nopful-litzod
ack
```

### Layers

| header | library | what |
| --- | --- | --- |
| `mesa/engine.h` | libmesa | the protocol engine: flows, scries, serving; sans-IO |
| `mesa/gall.h` | libmesa | gall's request/response encoding over flows |
| `mesa/store.h` | libmesa | pluggable transactional store; SQLite and in-memory |
| `mesa/ident.h` | libmesa | ships, keyfiles, fake keys, comet mining |
| `mesa/azimuth.h` | libmesa | azimuth-rpc `getPoint`/`getDns` encoding |
| `mesa/noun.h` | libmesa | nouns, `jam`/`cue`/`mug` |
| `mesa/host.h` | libmesa-host | libuv loop, UDP, galaxy DNS, azimuth over HTTPS |

The engine never does I/O: the host feeds it datagrams (`mesa_engine_hear`),
timer expirations (`mesa_engine_wake`, `mesa_engine_deadline`) and peer
keys (`mesa_engine_point` in answer to `hooks.need_point`), and it calls
`hooks.send`. libmesa-host is one such host; write your own to use another
event loop.

## Exactly-once delivery

Every input to the engine runs as one transaction against the store. The
application's handlers (`hooks.plea`, `hooks.boon`, `hooks.done`) run
inside it, and datagrams are released only after it commits. So:

- An incoming message is acked only after the transaction that handled it
  commits. A crash before that loses both the application's change and the
  ack, and the peer resends. A crash after it means the peer gets the ack
  on its next resend, and the message is not handled again.
- A plea handler that returns `MESA_NACK` has its writes rolled back (a
  savepoint) before the nack and its error are recorded.
- Messages the application sends (`mesa_flow_plea`, `mesa_flow_boon`,
  `mesa_gall_*`) are queued in the same transaction as the application's
  own state; wrap both in `mesa_txn_begin`/`mesa_txn_commit`.

This holds for the application's state when it lives in the same store:
with SQLite, use `mesa_store_sqlite_db()` and keep your tables in that
database. Side effects outside it (HTTP calls, files) can't be undone, so
they can repeat after a crash; record them in a table (an outbox) and
perform them after the commit.

`mesa_flow_done()` lets a handler return `MESA_LATER` and ack later, from
its own transaction.

### State and identity

Peers keep per-ship state (sequence numbers, acks), so an identity must
never run from two copies of its state, nor from fresh state once it has
talked to anyone:

- the SQLite store takes an exclusive lock on `<db>.lock`;
- `mesa_engine_open` refuses an empty store (`MESA_E_FRESH`) unless the
  configuration says `breached`: the identity never ran before, or its
  rift went up since. Comets start fresh on their own.
- It refuses a store that belongs to another ship or a later life or rift
  (`MESA_E_IDENTITY`).

A planet that previously ran as a real ship (classic ames) has peers that
remember it there; they ignore mesa from it until it breaches.

## Identities

- **Planets, stars, galaxies:** a keyfile (`@uw` jammed `+feed` or
  `+seed`), as `urbit -k` takes. libmesa-host checks the keys against
  azimuth at startup and learns the sponsor and rift there.
- **Moons:** the keyfile from the parent's `|moon`. Other ships learn a
  moon's keys from its parent planet, which must be online. Moons as
  *peers* need their keys supplied by the application (they're not on
  azimuth).
- **Comets:** `mesa_comet_mine()` under a list of live stars
  (bootstrap.urbit.org/comet-stars.jam), as `urbit -c` does; save the
  result with `mesa_keyfile_render()`. libmesa serves the comet's
  attestation (`/pawn/proof`) and verifies other comets'.
- **Fake ships:** `mesa_ident_fake()`, matching `urbit -F`.

## Build

Requires zig 0.15.2 (the same version vere uses).

```
zig build                      # zig-out/lib/, zig-out/bin/mesa-ping
zig build test -Dno-host=true  # all unit tests, without building the host
zig build -Doptimize=ReleaseFast
```

`libmesa.a` needs `libmesa-urcrypt.a` and `libmesa-sqlite.a`;
`libmesa-host.a` also needs libuv, curl and OpenSSL (built from the same
pinned sources vere uses, in `ext/`). Only the BLAKE3, ChaCha
(monocypher) and ed25519 parts of urcrypt are built.

## Tests

`zig build test` runs:

- `noun`, `crypto`, `ident`, `engine`: byte-for-byte against values
  computed by Hoon (`urbit eval`), including a full ack page (sealed path,
  ciphertext, MAC) and LSS roots;
- `engine`: engines on a simulated network: pleas, nacks and
  naxplanations, boons, 50KB messages, remote scry, corks, deferred acks,
  30% packet loss, crash-restarts, comets, and 200k garbage datagrams;
- `store`: both stores' semantics, boot checks, and exactly-once delivery
  into an application table across 12 crash-restarts on a lossy network;
- `pact`, `lss`, `bitset`, `ship`, `azimuth`, `gall`.

All of them are also clean under ASan, LeakSanitizer and UBSan.

### Interop test

`test/e2e/` runs libmesa against real Arvo. Boot a fake `~zod` whose base
desk includes `test/e2e/mesa-test.hoon` (add `%mesa-test` to `desk.bill`):

```
cp -rL urbit/pkg/arvo arvo
cp test/e2e/mesa-test.hoon arvo/app/ && echo add %mesa-test to arvo/desk.bill
urbit -F zod -A arvo -B solid.pill -c zod -t -x && urbit -t zod &
zig build e2e && zig-out/bin/mesa-e2e            # as ~nec
zig-out/bin/mesa-e2e -s bud.db -i ~bud           # as ~bud, persistent
```

It pokes (ack, crash → nack with naxplanation), watches and receives
facts, sends and receives 6KB multi-fragment messages, gets poked back by
`~zod`, remote-scries a grown path, leaves and corks.

## Layout

```
src/
  eng/          the engine (a port of the |mesa core of ames.hoon plus
                the packet layer of vere's mesa.c)
    engine.c    boot, inputs, timers, ping, the public API
    eng_flow.c  flows: pleas, boons, acks, naxplanations, corks
    eng_fetch.c requests: the .pit, multi-fragment downloads, retries
    eng_serve.c our namespace: pages, LSS proofs, the message cache
    eng_peer.c  peers: keys, routes, sponsorship, comet attestation
    eng_space.c namespace paths, page authentication and decryption
    eng_util.c  outbound queue, transactions, store keys
  host/host.c   libmesa-host
  store_mem.c, store_sqlite.c
  gall.c        gall request/response encoding
  noun.c        nouns: refcounting, jam/cue/mug/eq (iterative)
  crypto.{c,h}  ames.hoon ++crypt
  sha.{c,h}     +shay/+shax/+shal/+shas/+shaf
  ident.c       keyfiles, fake keys, comets
  knot.{c,h}    @ud/@uv/@p knots, paths, +en-beam
  json.{c,h}, azimuth.c
  ship.{c,h}, util.c, compat.h, arena.h
  wire/         packets (pact.c), LSS (lss.c), bitsets: from vere
include/mesa/   public headers
tools/          mesa-ping
test/           unit tests; e2e/ interop test
vendor/         c3 (from vere/pkg/c3), murmur3, verstable
ext/            libuv, curl, OpenSSL, nasm build wrappers (from vere)
```

The extracted files keep vere's naming and idioms on purpose, so changes can
flow back to vere.

## Deviations from vere and Arvo

- `pact.c`: `log_name()` renders `@p` in C, not via Hoon `scot`. The
  `PACT_TEST` section no longer boots the ivory pill; the Hoon `stab` path
  comparison was dropped. A name with the "initial" bit set and auth or
  fragment bits now fails to parse; vere `assert`s on these
  attacker-controlled bits.
- `lss.c`: shifts by a variable height are now unsigned (`1<<31` was
  undefined in `lss_builder_finalize`). Added `lss_recover_root()`: pages
  are authenticated over the message's BLAKE3 hash (`+recover-root`), and
  vere's `lss_root()` omits BLAKE3's ROOT flag, so it isn't that hash.
- `noun.c` (new, replaces u3 nouns): jam's dedup table hashes with a 64-bit
  structural hash rather than `+mug`. Iterating the 31-bit `+mug` up a deep
  spine cycles after ~2^15.5 levels, which made jam quadratic on deep
  left-nested nouns. jam's output doesn't depend on the hash.
- Retransmission backs off from 1s to ~m2 per request; vere retries twice
  at 1s and Arvo every ~m2. Timing only; the protocol is unchanged.
- No forwarding: a libmesa galaxy doesn't relay for its children.
- Not yet: `%shut` (group-encrypted) scry, `%flub`/`%halt` handling, moon
  keys from the parent planet, IPv6.

### Vere issues found along the way

1. `mesa.c` asks Arvo to authenticate a multi-fragment message's first
   page with a `%veri` scry whose path Arvo never answers, and treats the
   miss (`~`, which is `c3y`) as success. The early check is a no-op; Arvo
   re-authenticates the complete message, so the cost is downloading
   forged messages in full. A real answer (a cell) would stall downloads.
2. `pact.c` aborts on a crafted name (above).
3. `lss_root()` is not the signed root (above); fixing 1 alone would make
   every multi-fragment download fail authentication.

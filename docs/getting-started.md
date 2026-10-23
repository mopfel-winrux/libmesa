# Getting started

This walks through a first libmesa application in C: build the library,
pick an identity, open a store and a host, poke and watch an agent on
another ship, and be poked in turn. Every snippet uses the public headers
in `include/mesa/`. Three programs in the tree do all of this for real and
are worth reading alongside:

- `tools/mesa-ping.c`: pokes, watches, remote-scries, or listens for pokes.
- `tools/cli.c`: the identity and store options the tools share (keyfile,
  comet mining, fake ships).
- `examples/guestbook.c`: an agent other ships poke, watch and scry, with
  its own tables in the engine's database and a webhook outbox.

Other documents: [host.md](host.md) for driving the engine from your own
event loop, [store.md](store.md) for writing your own store,
[urbit-apps.md](urbit-apps.md) for talking to real Urbit apps (marks, gall
pleas, Tlon DMs), and [python.md](python.md) for the Python binding.

## Building

libmesa needs zig 0.15.2:

```
~/zig-x86_64-linux-0.15.2/zig build                         # debug, with UBSan
~/zig-x86_64-linux-0.15.2/zig build -Doptimize=ReleaseFast
~/zig-x86_64-linux-0.15.2/zig build -Dno-host=true          # core only: no libuv, curl, OpenSSL
~/zig-x86_64-linux-0.15.2/zig build test -Dno-host=true     # unit tests
```

Everything is installed under `zig-out/` (`-p dir` picks another prefix):

| file | what |
| --- | --- |
| `lib/libmesa.a` | the core: engine, gall encoding, nouns, identities, both stores |
| `lib/libmesa-urcrypt.a`, `lib/libmesa-sqlite.a` | what the core links against |
| `lib/libmesa.so` (`.dylib`) | the core with urcrypt and SQLite inside, for FFIs; no host |
| `lib/libmesa-host.a` | the ready-made host: libuv loop, UDP, galaxy DNS, azimuth over HTTPS |
| `lib/libmesa-uv.a`, `libmesa-curl.a`, `libmesa-ssl.a`, `libmesa-crypto.a` | what the host links against: libuv, curl and OpenSSL 1.1.1, renamed so they can't clash with the system's |
| `lib/pkgconfig/libmesa.pc`, `libmesa-host.pc` | pkg-config files; relocatable |
| `include/mesa/*.h` | the public headers; also `sqlite3.h`, `urcrypt.h`, `uv.h` |
| `bin/mesa-ping`, `bin/mesa-guestbook` | the example tools |

The prefix is self-contained: copy or unpack it anywhere. Release
tarballs on GitHub are this directory, built for x86_64 and aarch64
Linux (glibc 2.28 or later) and macOS.

The core has no I/O of its own. Almost every application wants the host
too, unless it drives the engine itself (see [host.md](host.md)).

### Linking

With pkg-config, pointing `PKG_CONFIG_PATH` at the prefix's
`lib/pkgconfig`:

```
export PKG_CONFIG_PATH=$PWD/zig-out/lib/pkgconfig
cc app.c $(pkg-config --cflags --libs --static libmesa-host)   # with the host
cc app.c $(pkg-config --cflags --libs --static libmesa)        # the core only
```

`-lmesa` finds `libmesa.so` if it's there and the linker prefers shared
libraries (most do); then the program needs it at run time (an rpath, or
`LD_LIBRARY_PATH`). Delete or move the `.so` for a fully static link, or
name the archives yourself:

```
cc app.c -Izig-out/include \
   zig-out/lib/libmesa-host.a zig-out/lib/libmesa.a \
   zig-out/lib/libmesa-urcrypt.a zig-out/lib/libmesa-sqlite.a \
   zig-out/lib/libmesa-curl.a zig-out/lib/libmesa-ssl.a \
   zig-out/lib/libmesa-crypto.a zig-out/lib/libmesa-uv.a \
   -lm -lpthread -ldl
```

(the core alone is the middle line plus `-lm -lpthread`). curl is built
against the pinned OpenSSL 1.1.1 that vere uses; a system OpenSSL 3 does
not satisfy it, so link the bundled one. `test/install/check.sh PREFIX`
builds and runs a program each way against an installed prefix.

The archives are built without LTO, so any C compiler links them. `zig
build -Dlto=true` builds libuv, curl and OpenSSL with LTO, as vere does;
those archives then hold LLVM bitcode, and only zig can link them.

Or build your application the way the tools are built: add an executable
to `build.zig` that links `host`, `lib`, `crypt`, `sql`, libuv and curl,
as the `tools` loop there does for `tools/mesa-ping.c`.

## Choosing an identity

An identity is a `mesa_ident`: ship, rift, life and a 65-byte suite `%b`
ring. There are four ways to get one (`mesa/ident.h`).

**Planets, stars, galaxies** load the keyfile `urbit -k` takes (`@uw` text
of a jammed `+feed` or `+seed`). `mesa_keyfile_parse` returns how many
lives it found, or -1:

```c
static int
load_identity(const char* path, mesa_ident* ide)
{
  FILE*  f = fopen(path, "r");
  char   txt[8192] = {0};
  size_t n;

  if ( !f ) return 0;
  n = fread(txt, 1, sizeof(txt) - 1, f);
  fclose(f);
  return n && (mesa_keyfile_parse(txt, ide, 1) > 0);
}
```

A keyfile that doesn't carry the rift leaves it `MESA_RIFT_UNKNOWN`.
libmesa-host fetches our point from azimuth at startup, fills in the rift
and sponsor, and fails with `MESA_HOST_E_KEYS` if the life or keys don't
match what azimuth has, or `MESA_HOST_E_AZIMUTH` if it can't get the point.

**Moons** use the keyfile their parent's `|moon` prints, parsed the same
way. They are not on azimuth: the host skips the check, our sponsor is the
parent, and other ships learn our keys from the parent, which must be
online.

**Comets** are mined under a live star, as `urbit -c` does. The list of
stars is `https://bootstrap.urbit.org/comet-stars.jam`, a jammed list of
`@p`. Mining needs 64 bytes of fresh randomness. This is `_mine()` from
`tools/cli.c`:

```c
#include <curl/curl.h>
#include <sys/random.h>

static int
mine_comet(mesa_ident* ide)
{
  size_t    len, n = 0;
  uint8_t*  buf = mesa_host_get("https://bootstrap.urbit.org/comet-stars.jam", &len);
  mesa_noun lis;
  mesa_ship stars[4096];
  uint8_t   eny[64];

  if ( !buf ) return 0;
  lis = mesa_cue_bytes(len, buf);
  mesa_free(buf);
  if ( MESA_NONE == lis ) return 0;
  for ( mesa_noun i = lis; mesa_is_cell(i) && n < 4096; i = mesa_tail(i) ) {
    uint64_t s;
    if ( mesa_atom_get_u64(mesa_head(i), &s) ) stars[n++] = (mesa_ship){ .lo = s };
  }
  mesa_unref(lis);

  if ( 64 != getrandom(eny, sizeof(eny), 0) ) return 0;
  return mesa_comet_mine(n, stars, eny, UINT64_MAX, ide);
}
```

`mesa_host_get` is a blocking HTTPS GET from libmesa-host; call
`curl_global_init(CURL_GLOBAL_DEFAULT)` first, as the tools do. A comet is
only worth keeping together with its state, so save its keys next to the
database and load them on the next run with `load_identity` above:

```c
#include <fcntl.h>

static void
save_comet(const char* path, const mesa_ident* ide)
{
  char* txt = mesa_keyfile_render(ide);   //  a [%2 ~] feed
  int   fd  = open(path, O_WRONLY | O_CREAT | O_EXCL, 0600);   //  it's a secret
  FILE* f   = ( fd < 0 ) ? 0 : fdopen(fd, "w");
  if ( f ) { fprintf(f, "%s\n", txt); fclose(f); }
  mesa_free(txt);
}
```

**Fake ships** talk to `urbit -F` ships on localhost. Their keys come
from their name:

```c
mesa_ship who;
mesa_ship_parse("~nec", &who);
mesa_ident_fake(who, &cfg.engine.ident);
cfg.fake            = true;     //  galaxy ~g is at 127.0.0.1:31337+g
cfg.engine.breached = true;     //  see below
```

### `breached`, `MESA_E_FRESH` and `MESA_E_IDENTITY`

Peers keep per-ship state (sequence numbers, acks), so an identity must
never start over from empty state once it has talked to anyone. Opening
the engine checks the store against the identity:

- An empty store fails with `MESA_E_FRESH` unless `cfg.engine.breached` is
  set. Set it only when the identity has never run anywhere, or its rift
  went up (a breach, a factory reset) since it last ran. Comets are exempt.
  Fake ships are not: set it for them, as `-F` does in `tools/cli.c`.
- A store that belongs to another ship, or to a later life or rift than
  the identity, fails with `MESA_E_IDENTITY`. So does a store with rows
  but no identity.
- A store from an earlier rift fails with `MESA_E_FRESH` unless
  `breached` is set; with it, the old protocol state is wiped and the
  identity starts over.

A planet you have been running in Urbit cannot move to libmesa as is:
breach it first and start libmesa with `breached`. The volatile
`mesa_store_memory()` is always empty, so it always needs `breached` (the
tools set it when there is no `-s`).

## Opening the store and the host

```c
#include "mesa/gall.h"
#include "mesa/host.h"

static mesa_host*   hos;
static mesa_engine* eng;

int
main(void)
{
  mesa_host_config cfg = {0};
  const char*      err;
  int              ret;

  curl_global_init(CURL_GLOBAL_DEFAULT);

  if ( !load_identity("app.key", &cfg.engine.ident) ) {
    if ( !mine_comet(&cfg.engine.ident) ) return 1;
    save_comet("app.key", &cfg.engine.ident);
  }

  if ( !(cfg.engine.store = mesa_store_sqlite("app.db", &err)) ) {
    fprintf(stderr, "app.db: %s\n", err);
    return 1;
  }

  cfg.engine.hooks = (mesa_hooks){ .ctx  = 0,
                                   .done = on_done, .boon = on_boon,
                                   .sage = on_sage, .plea = on_plea,
                                   .peek = on_peek, .breach = on_breach };

  if ( MESA_OK != (ret = mesa_host_open(&cfg, &hos)) ) {
    fprintf(stderr, "can't start: %d\n", ret);   //  mesa_err or mesa_host_err
    return 1;
  }
  eng = mesa_host_engine(hos);

  //  ...start flows, then mesa_host_kick(hos)...

  mesa_host_run(hos);              //  until mesa_host_stop()
  mesa_host_close(hos);
  mesa_store_free(cfg.engine.store);
  return 0;
}
```

`mesa_store_sqlite` creates the database if needed and takes an exclusive
lock on `app.db.lock`; a second process opening the same file gets NULL
and an error. The store belongs to you: close the host first, then free
the store.

`mesa_host_open` resolves our own point (unless we're fake, a comet or a
moon), binds a UDP socket (galaxies on their well-known port, everyone
else on an ephemeral one, unless `cfg.port` says), and opens the engine. It fills in `hooks.send` and `hooks.need_point` and passes the other
hooks through with your `ctx`. Any hook you leave NULL is simply not
called; without `plea`, every request to us is dropped.

`mesa_host_run` is `uv_run` on the default loop, or on `cfg.loop` if you
set one. Everything, hooks included, runs on that thread. Add your own
libuv handles to `mesa_host_loop(hos)`; `examples/guestbook.c` serves HTTP
and drains an outbox that way. Call `mesa_host_stop` from a hook or
handle to make `mesa_host_run` return, as `mesa-ping` does after the ack.

Other `mesa_host_config` fields: `azimuth_url` (default
`MESA_AZIMUTH_URL`) and `dns_domain` (default `urbit.org`; galaxy `~zod`
is `zod.urbit.org`). `cfg.engine.no_ping` turns off the 25-second ping to
our galaxy that keeps its route to us fresh.

## Talking to an agent

Messages travel on flows, Arvo's bones. A forward flow carries our
requests to a peer and their responses back; messages on a flow are
delivered in order, one at a time. Open one with `mesa_flow_open`. One
flow can carry any number of pokes; each subscription needs its own.

### Poking

```c
mesa_ship zod;
uint64_t  bone;

mesa_ship_parse("~zod", &zod);
mesa_flow_open(eng, zod, &bone);
mesa_gall_poke(eng, zod, bone, "ping", "noun", mesa_atom_u64(0));
mesa_host_kick(hos);
```

The poke-ack or poke-nack arrives in `hooks.done`. `error` is `MESA_NONE`
for an ack; for a nack it is the naxplanation, `[term tang]`, where the
term is usually the crash tag and the tang a stack trace (`mesa-ping.c`
has a `+ram`-style tank printer):

```c
static void
on_done(void* ctx, mesa_ship her, uint64_t bone, mesa_noun error)
{
  if ( MESA_NONE == error ) {
    printf("bone %llu: ack\n", (unsigned long long)bone);
  }
  else {
    char* tag = mesa_atom_get_cord(mesa_head(error));
    printf("bone %llu: nack %%%s\n", (unsigned long long)bone, tag);
    mesa_free(tag);
  }
}
```

A nack doesn't stop the flow; the next message on it goes out as usual.
If the agent isn't running, the peer's gall drops our poke and tells us
so (`%flub`); the engine stops resending on that flow until the agent
comes back (`%spur`), and `done` fires only then.

What to put in the mark and noun for a real app is in
[urbit-apps.md](urbit-apps.md).

### Watching

A watch path is a noun list of cords. `mesa-ping` builds one from a
string:

```c
static mesa_noun
path_noun(const char* str)          //  "/a/b" -> ['a' 'b' ~]
{
  char*     dup = strdup(str);
  char*     seg[64];
  int       n = 0;
  mesa_noun pax = mesa_atom_u64(0);

  for ( char* t = strtok(dup, "/"); t && n < 64; t = strtok(0, "/") ) seg[n++] = t;
  while ( n-- ) pax = mesa_cell(mesa_atom_cord(seg[n]), pax);
  free(dup);
  return pax;
}

uint64_t sub;
mesa_flow_open(eng, zod, &sub);
mesa_gall_watch(eng, zod, sub, "my-agent", path_noun("/updates"));
mesa_host_kick(hos);
```

The watch-ack (or nack) arrives in `hooks.done` on `sub`. Facts and kicks
arrive in `hooks.boon`; decode them with `mesa_gall_response`:

```c
static void
on_boon(void* ctx, mesa_ship her, uint64_t bone, mesa_noun boon)
{
  mesa_noun mark, noun;

  switch ( mesa_gall_response(boon, &mark, &noun) ) {
    case MESA_GALL_FACT: {
      char* m = mesa_atom_get_cord(mark);
      printf("fact %%%s on bone %llu\n", m, (unsigned long long)bone);
      mesa_free(m);
    } break;
    case MESA_GALL_KICK: printf("kicked\n"); break;
    default: break;
  }
}
```

To unsubscribe, do what gall does: leave, and once the leave is acked
(`hooks.done` on that bone), cork the flow. When the cork is acked the
flow is gone for good and `hooks.corked` fires (not `done`); further pleas
on it fail with `MESA_E_STATE`.

```c
mesa_gall_leave(eng, zod, sub, "my-agent");
/* ...in on_done for sub: */
mesa_flow_cork(eng, zod, sub);
```

Flows persist in the store, so after a restart facts keep arriving on the
same bone. Record the bone in your own table, in the same transaction as
`mesa_flow_open` (see below), so you know what it belongs to.

### Remote scry

`mesa_keen` reads a path from a peer's public namespace; the answer
arrives in `hooks.sage` as `[mark noun]`:

```c
mesa_keen(eng, zod, "/g/x/1/my-agent//1/hello");
mesa_host_kick(hos);

static void
on_sage(void* ctx, mesa_ship her, const char* path, mesa_noun gage)
{
  if ( MESA_NONE == gage ) return;        //  cancelled: the peer breached
  char* m = mesa_atom_get_cord(mesa_head(gage));
  printf("%s: %%%s\n", path, m);
  mesa_free(m);
}
```

The engine retries until it gets an answer; a second `mesa_keen` for a
path already pending does nothing, and `mesa_yawn` gives up on one
without calling `sage`. Pending reads live in memory, not in the store:
after a restart, ask again.

## Being poked

Requests from other ships arrive in `hooks.plea` on our backward flow
`bone`. For gall requests, `mesa_gall_request_parse` decodes the vane,
path and payload into a `mesa_gall_request`: the agent name, and the
mark, noun or path, all borrowed from the plea. The handler says what to
do with the message:

- `MESA_ACK`: handled; ack it.
- `MESA_NACK`: refused. Its writes to the store are rolled back (this
  needs a store with savepoints, as both built-in ones have; see
  [store.md](store.md#savepoints)), and
  `*error` (a `[tag=@tas tang]` noun you build; `MESA_NONE` for a
  default) goes back as the naxplanation.
- `MESA_LATER`: no answer yet. Call `mesa_flow_done(eng, her, bone,
  error)` later, from your own transaction, with `MESA_NONE` to ack or an
  error to nack. Later messages on that flow wait until then.
- `MESA_DROP`: not ours; no answer at all, writes rolled back. The peer
  keeps resending, as it would to an Arvo ship whose agent isn't running,
  so the message is delivered if an application that does run that agent
  later takes over this identity.

Drop anything you don't recognise. (Gall's own `/gf` and `/gk` flows,
`MESA_GALL_SYSTEM`, are answered by the engine and never reach the hook.)

```c
static mesa_plea_result
on_plea(void* ctx, mesa_ship her, uint64_t bone, mesa_noun vane,
        mesa_noun path, mesa_noun payload, mesa_noun* error)
{
  mesa_gall_request req;

  switch ( mesa_gall_request_parse(vane, path, payload, &req) ) {
    case MESA_GALL_POKE: {
      if ( strcmp(req.agent, "echo") ) return MESA_DROP;
      if ( !mesa_is_atom(req.noun) ) {
        *error = mesa_cell(mesa_atom_cord("bad-poke"), mesa_atom_u64(0));
        return MESA_NACK;
      }
      //  ...record it...
      return MESA_ACK;
    }

    case MESA_GALL_WATCH:
      if ( strcmp(req.agent, "echo") ) return MESA_DROP;
      //  ...remember (her, bone) as a subscriber...
      return MESA_ACK;

    case MESA_GALL_LEAVE:
      //  ...forget (her, bone)...
      return MESA_ACK;

    default: return MESA_DROP;
  }
}
```

`guestbook.c` builds a real tang for its nacks (`_error()`: `[tag
~[leaf+msg]]`), validates the watch path, and keeps subscribers in a
table.

To a subscriber, send facts on the bone their watch came in on, from the
`plea` hook or from anywhere else; kick with `mesa_gall_kick`:

```c
mesa_gall_fact(eng, sub_ship, sub_bone, "noun", mesa_atom_cord("hello"));
```

When a peer breaches, all its flows with us are gone; `hooks.breach` is
where to drop its subscriptions.

### Serving scries

`hooks.peek` serves our namespace. `path` is the inner path, e.g.
`/g/x/1/echo//1/hello`; `her` is NULL for a public read and the reader for
an encrypted two-party one. Return a new `[mark noun]` cell, or
`MESA_NONE` if there is nothing there. A path, once bound, must always
have the same value: serve immutable data, like `guestbook.c`'s entry
*n* at `/g/x/<n>/guestbook//1/entry`.

```c
static mesa_noun
on_peek(void* ctx, const mesa_ship* her, const char* path)
{
  if ( !strcmp(path, "/g/x/1/echo//1/hello") ) {
    return mesa_cell(mesa_atom_cord("noun"), mesa_atom_cord("world"));
  }
  return MESA_NONE;
}
```

## Exactly once

Every input to the engine (a datagram, a timer, a call to the API) runs
as one transaction against the store. The `plea`, `boon` and `done` hooks
run inside it, and datagrams leave only after it commits. A message is
therefore acked only once the transaction that handled it has committed:
a crash before the commit loses both your change and the ack, and the
peer resends; a crash after it means the peer gets the ack on its next
resend and the message is not handled again (the full argument is in
[store.md](store.md#why-its-exactly-once)).

That covers your own state only if it lives in the same database. With
the SQLite store, get its handle and keep your tables there, named so
they don't collide with libmesa's `mesa_kv`:

```c
#include <sqlite3.h>

sqlite3* db = mesa_store_sqlite_db(cfg.engine.store);
sqlite3_exec(db, "CREATE TABLE IF NOT EXISTS app_msg ("
                 "  id INTEGER PRIMARY KEY, ship TEXT, text TEXT)", 0, 0, 0);
```

Inside a hook, just write: you are already in the transaction. Don't
begin one there (`mesa_txn_begin` returns `MESA_E_STATE`).

Outside a hook (at startup, in a timer, in a libuv callback), group your
writes and engine calls with `mesa_txn_begin`/`mesa_txn_commit` so they
commit together, or `mesa_txn_abort` to undo both. Don't issue your own
`BEGIN` on the handle; the engine owns the transaction. Then call
`mesa_host_kick`, so the host re-arms its timer for whatever you queued:

```c
if ( MESA_OK != mesa_txn_begin(eng) ) return;
mesa_flow_open(eng, zod, &sub);
mesa_gall_watch(eng, zod, sub, "my-agent", path_noun("/updates"));
{
  sqlite3_stmt* stm;
  sqlite3_prepare_v2(db, "INSERT INTO app_sub (ship, bone) VALUES ('~zod', ?)",
                     -1, &stm, 0);
  sqlite3_bind_int64(stm, 1, (int64_t)sub);
  sqlite3_step(stm);
  sqlite3_finalize(stm);
}
if ( MESA_OK != mesa_txn_commit(eng) ) { /* the engine has stopped */ }
mesa_host_kick(hos);
```

An engine call made outside a hook and outside a transaction runs in one
of its own. Either way, the kick is what makes retransmission happen;
without it the first datagram goes out but nothing resends it if it is
lost. Inside hooks the host kicks for you.

If the store fails, the engine stops (`MESA_E_FAILED` from then on): close
it and open a new one on the same store, which resumes from the last
commit ([store.md](store.md#failure)).

Side effects outside the database (HTTP calls, files, other services)
can't be rolled back and may repeat after a crash. Write them to an
outbox table in the transaction, and perform them after it commits.
`guestbook.c` does this for its webhook: `_sign()` inserts into
`gb_outbox`, a `uv_check_t` drains it after each loop iteration, the POST
runs on the thread pool (`uv_queue_work` and `mesa_host_post`), and the
row is deleted in its own `mesa_txn_begin`/`mesa_txn_commit` once
delivered. That is at least once, with an id the receiver can
deduplicate on. `test/guestbook/crash.sh` checks it under `kill -9`.

## Nouns: who owns what

Nouns are refcounted (`mesa/noun.h`), and every call either takes your
reference or borrows it:

- Constructors (`mesa_cell`, `mesa_trel`, `mesa_qual`) and everything you
  pass to the engine (`mesa_flow_*`, `mesa_gall_poke/watch/fact`, a
  plea's `*error`, the cell `peek` returns) **take** the reference. Don't
  use or unref the noun afterwards. This holds when the call fails, too.
- Accessors (`mesa_head`, `mesa_tail`, `mesa_at`), the outputs of
  `mesa_gall_response` and `mesa_gall_request_parse`, and every noun a
  hook receives are **borrowed**: valid until the parent is freed or the
  hook returns. `mesa_ref` one to keep it or to hand it on.
- Everything else (`mesa_jam`, `mesa_eq`, `mesa_atom_get_cord`, ...)
  borrows its arguments.

```c
//  built and handed over: nothing to free
mesa_gall_poke(eng, zod, bone, "my-agent", "noun",
               mesa_cell(mesa_atom_cord("hi"), mesa_atom_u64(0)));

//  to use a noun twice, take a second reference
mesa_noun msg = mesa_atom_cord("hello");
mesa_gall_fact(eng, a, abone, "noun", mesa_ref(msg));
mesa_gall_fact(eng, b, bbone, "noun", msg);          //  last use: hand it over

//  echoing a poke: req.noun is borrowed from the plea, so ref it
mesa_gall_fact(eng, her, bone, "noun", mesa_ref(req.noun));

//  keeping a fact past the hook
static mesa_noun last = MESA_NONE;
if ( MESA_NONE != last ) mesa_unref(last);
last = mesa_ref(noun);

//  something you made and didn't hand over: unref it
mesa_noun pax = path_noun("/updates");
uint8_t*  buf = mesa_jam_bytes(pax, &len);   //  borrows pax
mesa_unref(pax);
mesa_free(buf);
```

Strings and buffers libmesa returns (`mesa_atom_get_cord`,
`mesa_jam_bytes`, `mesa_ship_render`, `mesa_keyfile_render`,
`mesa_host_get`) are freed with `mesa_free`. Nouns are not thread-safe;
keep them on the loop thread.

There is no helper for a ship as a noun; `guestbook.c`'s `_ship_noun()`
packs `mesa_ship.lo`/`.hi` into a 16-byte atom.

## Logging and tracing

libmesa logs to stderr. `mesa_set_log` (`mesa/log.h`) sends each line to
your function instead; it is process-wide, so set it before opening
anything:

```c
#include "mesa/log.h"

static void
log_line(void* ctx, const char* line)
{
  fprintf(stderr, "[mesa] %s\n", line);
}

mesa_set_log(log_line, 0);
```

`MESA_TRACE=1` in the environment makes the engine log the name of every
packet it sends and hears (peeks, pages, pokes, with ship, rift, fragment
and path), and libmesa-host log every datagram and its lane:

```
MESA_TRACE=1 zig-out/bin/mesa-ping -F ~nec ~zod ping hi
```

The host's HTTPS calls look for a CA bundle in the usual places;
`MESA_CA_FILE` names one explicitly.

## Next

Run `mesa-ping -F ~nec ~zod ping hi` against a fake `~zod`
(`urbit -F zod`) to see a poke end to end, then `mesa-guestbook -F ~nec
-s gb.db` and sign it from `~zod`'s dojo with `:~nec/guestbook
'hello!'`. From there, [urbit-apps.md](urbit-apps.md) covers the marks and
pokes real Urbit apps expect.

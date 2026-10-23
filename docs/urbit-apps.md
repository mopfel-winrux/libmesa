# Talking to real Urbit apps

libmesa runs no Hoon. To talk to a gall agent on a real ship you send it
the same messages another ship's gall would, with nouns you build in C
(or Python), and you answer its messages the same way. This page covers
what those messages are, how to build them, and two worked examples: Tlon
DMs and the guestbook.

See also [getting-started.md](getting-started.md) (a first program),
[host.md](host.md) (the libuv host, or writing your own),
[store.md](store.md) (transactions and exactly-once delivery) and
[python.md](python.md) (the ctypes binding).

## Flows

Everything between two ships travels on *flows*, as in Arvo's ames. A
flow is a bone, a number unique per peer (`mesa_flow_open()` hands out
multiples of 4). Each flow has two ends:

- a **forward** flow is one we opened: it carries our *pleas* (requests)
  to the peer, and the peer's *boons* (responses) back;
- a **backward** flow is one the peer opened: it carries their pleas to
  us, and our boons to them.

A plea is `[vane path payload]`: `vane` is a `@tas` such as `%g` (gall)
or `%j` (jael), `path` says what it's for (`/ge/<agent>` for gall), and
`payload` is the vane's own request.

Pleas on one flow are delivered in order, one at a time: the next is sent
only after the previous one is acked or nacked. Use separate flows for
traffic that shouldn't wait on each other. Every plea gets exactly one
answer, reported to `hooks.done`:

- an **ack** (`error` is `MESA_NONE`): the receiver took it;
- a **nack** (`error` is the *naxplanation*): it refused, or crashed
  handling it. A naxplanation is `[tag=@tas tang]`: a term and a list of
  tanks, typically a stack trace. The flow carries on after a nack.

Boons have no acks visible to the application; the engine acks them and
calls `hooks.boon`. A flow is closed with `mesa_flow_cork()`, which is
itself a plea; when the peer acks it, `hooks.corked` runs (not `done`) and
the bone is gone for good (`mesa_flow_plea` on it returns `MESA_E_STATE` once the
cork is queued).

## Gall over flows

A gall request is a plea `[%g /ge/<agent> [%0 request]]`, where `request`
is one of gall's `$ames-request`:

| request | meaning | send with |
| --- | --- | --- |
| `[%m mark noun]` | poke | `mesa_gall_poke()` |
| `[%s path]` | watch (subscribe) | `mesa_gall_watch()` |
| `[%l mark path]` | watch-as | `mesa_flow_plea()` (no helper) |
| `[%u ~]` | leave | `mesa_gall_leave()` |

Responses come back as boons on the same flow (`$ames-response`):

| boon | meaning | decode with |
| --- | --- | --- |
| `[%d mark noun]` | a fact | `mesa_gall_response()` → `MESA_GALL_FACT` |
| `[%x ~]` | a kick: the subscription is over | `mesa_gall_response()` → `MESA_GALL_KICK` |

For a poke, the ack or nack in `hooks.done` is the poke-ack. For a watch,
it's the watch-ack; facts follow as boons.

```c
uint64_t bone;

mesa_flow_open(eng, her, &bone);
mesa_gall_poke(eng, her, bone, "my-agent", "noun", mesa_atom_cord("hi"));
```

Pokes can share a flow (they're then delivered in order). Each
subscription needs its own flow, as gall gives each one its own bone:
open a flow, send the watch on it, and keep the bone to recognize its
facts in `hooks.boon`.

```c
static void
on_boon(void* ctx, mesa_ship her, uint64_t bone, mesa_noun boon)
{
  mesa_noun mark, noun;     //  borrowed from boon

  switch ( mesa_gall_response(boon, &mark, &noun) ) {
    case MESA_GALL_FACT: /* a fact on subscription [bone] */ break;
    case MESA_GALL_KICK: /* subscription [bone] ended   */ break;
    default:             /* not gall's                   */ break;
  }
}
```

To unsubscribe, send `mesa_gall_leave()` on the subscription's bone, and
once the leave is acked, `mesa_flow_cork()` it, as gall does (the e2e test
does exactly this). A watch-as has no helper; build the plea yourself:

```c
mesa_flow_plea(eng, her, bone, mesa_atom_cord("g"),
               mesa_cell(mesa_atom_cord("ge"),
                         mesa_cell(mesa_atom_cord("my-agent"), 0)),
               mesa_cell(0, mesa_trel(mesa_atom_cord("l"),
                                      mesa_atom_cord("json"), path)));
```

(`mesa_atom_u64(0)` is `0`, so a literal `0` is `~`.)

## Marks

libmesa sends a raw noun tagged with a mark name; it knows nothing about
marks. The receiving gall checks the noun against that mark from the
agent's desk before the agent sees it. So:

- the mark must exist on the receiving agent's desk, and the noun must fit
  it. `%noun` accepts any noun, which is why `mesa-ping` pokes with
  `%noun`;
- a mark the desk doesn't have, a noun that doesn't fit, or an agent that
  rejects the poke all come back as a nack, with the naxplanation saying
  why (`mesa-ping ~zod hood hi` shows one);
- facts you receive carry the publisher's mark name and the raw noun:
  nothing converts them for you (a watch-as asks the publisher to convert
  before sending).

In practice, to talk to an app you must know the exact noun each of its
marks expects. Read the app's `sur/` types and `mar/` files.

## Building nouns in C

Nouns passed to libmesa are transferred; constructors take ownership of
their arguments (see `mesa/noun.h`). The shapes you need:

| Hoon | C |
| --- | --- |
| `~`, `%.y`, `0` | `mesa_atom_u64(0)` |
| `%.n` | `mesa_atom_u64(1)` |
| `'text'`, `%term` (cord, `@t`, `@tas`) | `mesa_atom_cord("text")` |
| `[a b]`, `[a b c]`, `[a b c d]` | `mesa_cell()`, `mesa_trel()`, `mesa_qual()` |
| `[%tag p]` | `mesa_cell(mesa_atom_cord("tag"), p)` |
| `[~ x]` (unit with a value) | `mesa_cell(0, x)` |
| `~[a b]` (list) | `mesa_cell(a, mesa_cell(b, 0))` |
| `/a/b` (path) | `mesa_cell(mesa_atom_cord("a"), mesa_cell(mesa_atom_cord("b"), 0))` |
| `"tape"` | a list of byte atoms |

Lists and paths are built back to front (this is how `mesa-ping -w`
builds its path):

```c
static mesa_noun
path_of(const char* const* seg, size_t n)    //  {"a", "b"} → /a/b
{
  mesa_noun pax = 0;
  while ( n-- ) pax = mesa_cell(mesa_atom_cord(seg[n]), pax);
  return pax;
}
```

A ship (`@p`) is just its number. `mesa_ship` is 128 bits, little-endian
in `lo`/`hi`; `mesa_atom_bytes()` trims the high zero bytes, so this gives
the same atom for any rank (from `examples/guestbook.c`):

```c
static mesa_noun
ship_atom(mesa_ship who)
{
  uint8_t buf[16];
  for ( int i = 0; i < 8; i++ ) {
    buf[i]     = (uint8_t)(who.lo >> (8 * i));
    buf[i + 8] = (uint8_t)(who.hi >> (8 * i));
  }
  return mesa_atom_bytes(16, buf);
}
```

A date (`@da`) is 128 bits: whole seconds since Urbit's epoch in the high
64, the fraction of a second in the low 64. Unix time 0 is
`0x8000000cce9e0d80` seconds, so
`@da = (0x8000000cce9e0d80 + unix_sec) << 64 | frac`, where `frac` is the
fraction times 2^64 (this is `da()` in `bindings/python/libmesa.py`):

```c
static mesa_noun
da_atom(uint64_t unix_sec, uint64_t frac)
{
  uint64_t sec = 0x8000000cce9e0d80ULL + unix_sec;
  uint8_t  buf[16];
  for ( int i = 0; i < 8; i++ ) {
    buf[i]     = (uint8_t)(frac >> (8 * i));
    buf[i + 8] = (uint8_t)(sec >> (8 * i));
  }
  return mesa_atom_bytes(16, buf);
}
```

Apps that use a `@da` as a message id (Tlon does) need it unique: bump it
when two would collide.

Reading nouns: `mesa_head()`, `mesa_tail()` and `mesa_at(axis, noun)`
return borrowed nouns (`mesa_at` gives `MESA_NONE` for a missing axis, so
it's safe on untrusted input). Check `mesa_is_cell()`/`mesa_is_atom()`
before taking a noun apart, and compare tags with `mesa_atom_get_cord()`
(free the result with `mesa_free()`).

### Reading a nack

The `error` in `hooks.done` is `[tag tang]`. A tang is a list of tanks;
a tank is `[%leaf tape]`, `[%rose [sep open close] (list tank)]` or
`[%palm [p q r s] (list tank)]`. `mesa-ping`'s `_done()`/`_tank()` print
one line per tank; the Python binding's `tang()` returns them as a list
of strings. A minimal reader handles `%leaf` only:

```c
static void
on_done(void* ctx, mesa_ship her, uint64_t bone, mesa_noun error)
{
  if ( MESA_NONE == error ) return;                 //  acked
  for ( mesa_noun t = mesa_tail(error); mesa_is_cell(t); t = mesa_tail(t) ) {
    mesa_noun tan = mesa_head(t);
    uint64_t  tag, c;
    if (  mesa_is_cell(tan) && mesa_atom_get_u64(mesa_head(tan), &tag)
       && (0x6661656c == tag) )                     //  %leaf
    {
      for ( mesa_noun p = mesa_tail(tan); mesa_is_cell(p); p = mesa_tail(p) ) {
        if ( mesa_atom_get_u64(mesa_head(p), &c) ) putchar((int)c);
      }
      putchar('\n');
    }
  }
}
```

To nack with an explanation of your own, build the same shape
(`examples/guestbook.c`, `_error()`):

```c
//  [tag ~[[%leaf msg]]]
mesa_noun tap = 0;
for ( size_t i = strlen(msg); i--; ) tap = mesa_cell(mesa_atom_u64((uint8_t)msg[i]), tap);
*error = mesa_cell(mesa_atom_cord("bad-entry"),
                   mesa_cell(mesa_cell(mesa_atom_cord("leaf"), tap), 0));
```

## Serving: being an agent

Other ships' gall pokes and watches "your agents" with the same pleas.
They arrive in `hooks.plea` on a backward flow; decode them with
`mesa_gall_request_parse()`:

| kind | request | fields |
| --- | --- | --- |
| `MESA_GALL_POKE` | `[%m mark noun]` | `agent`, `mark`, `noun` |
| `MESA_GALL_WATCH` | `[%s path]` | `agent`, `path` |
| `MESA_GALL_WATCH_AS` | `[%l mark path]` | `agent`, `mark`, `path` |
| `MESA_GALL_LEAVE` | `[%u ~]` | `agent` |
| `MESA_GALL_SYSTEM` | gall's `/gf`, `/gk` | none |
| `MESA_GALL_NONE` | anything else | |

The engine answers `/gf` and `/gk` itself, so `hooks.plea` never sees
`MESA_GALL_SYSTEM`; a `default:` case covers it. The handler returns:

- `MESA_ACK`: the poke-ack or watch-ack;
- `MESA_NACK`: refused; set `*error` to `[tag tang]`, or leave it
  `MESA_NONE` for `[%error ~]`. The handler's writes to the store are
  rolled back (with a store that has savepoints; see
  [store.md](store.md#savepoints));
- `MESA_LATER`: answer later with `mesa_flow_done()`;
- `MESA_DROP`: no answer at all.

Return `MESA_DROP` for agents you don't run. That is what Arvo does with
pleas for an agent that isn't running: the sender keeps resending, so if
an application that does run the agent takes over the identity later,
the message is delivered then. A nack would instead tell the sender the
agent refused it, permanently. (Arvo also sends the sender `%flub`; libmesa
never does. See the README's deviations.)

After acking a watch, send facts on the watcher's bone with
`mesa_gall_fact(eng, her, bone, mark, noun)`, from the plea handler (an
initial fact) or later, from your own transaction. Keep `(ship, bone)`
for each subscriber. `mesa_gall_kick()` ends a subscription from our side.
On a leave, ack and forget the subscriber; the peer then corks the flow,
which the engine handles, and `mesa_gall_fact` on it would return
`MESA_E_STATE`. When a peer breaches (`hooks.breach`), all its flows are
gone, so drop its subscriptions too.

A watch-as asks you to send facts with that mark. `mesa_gall_request_parse`
gives you the mark; what to do with it is up to you (the Python binding
treats a watch-as as a plain watch).

### Version negotiation

Some apps check, before talking to a ship, which version of their
protocol it speaks: they watch `/~/negotiate/version/<protocol>` on the
agent and expect a fact with the version. Answer such a watch with an ack
and a fact. Tlon's `%chat` does this (below).

## Remote scry

`mesa_keen(eng, her, path)` reads `path` from a ship's public namespace;
the answer arrives in `hooks.sage` as `[mark noun]`, or `MESA_NONE` if
the request was dropped because `her` breached. `mesa_yawn()` stops
asking, and no `sage` follows. Outstanding reads live in memory only:
ask again after a restart. Gall's paths look like
`/g/x/<case>/<agent>//<path>`. For example an agent that does
`[%pass /grow %grow /hello [%noun 'world']]` publishes revision 1 of
`/hello`, which reads as `/g/x/1/mesa-test//1/hello`
(`test/e2e/mesa-test.hoon`). `mesa-ping -x <path> ~ship` reads one.

To publish, implement `hooks.peek`: given the inner path (e.g.
`/g/x/3/guestbook//1/entry`), return a `[mark noun]` cell, or `MESA_NONE`
if there's nothing there. `her` is `NULL` for public reads and the reader
for two-party encrypted ones. Paths under `/a/` are the engine's own and
never reach the hook.

Bindings must be immutable: once a path has a value it must always have
that same value. Readers download a value in pages and check them against
the whole message, and anyone may cache it. Put a version or counter in
the path (the case) and publish new values at new paths, as `%grow` does.

## What the engine does for you

Arvo's vanes exchange some messages on their own behalf. The engine
speaks them, and `hooks.plea`/`boon`/`done` never see them
(`src/eng/eng_sys.c`, `src/eng/engine.c`, `src/eng/eng_ames.c`):

- **Halting on `%flub`/`%spur`.** Before our first gall plea to a ship,
  the engine opens a flow of its own and sends `[%g /gf [%0 ~]]`, asking
  to hear about that ship's agents. If we poke an agent that isn't
  running, the ship drops our plea and sends `[%0 %flub agent bone]`; the
  engine stops resending that flow. On `[%0 %spur agent]` it resumes every
  halted flow to that ship (one still out is flubbed again). The
  application sees nothing: no ack until the agent is back. (See the
  README's Arvo issue 4 for a bug on the receiving side.)
- **Their `/gf` and `/gk` to us.** `/gf` is acked (we never flub). A `/gk`
  key request for a `%shut` path is answered `[%0 ~]`, since we have no
  encrypted namespace; a bare ack would leave their scry hanging.
- **Moon keys.** There is no azimuth for moons. For a moon peer (not our
  own, and not on a fake network) the engine asks its parent's jael with
  `[%j /public-keys [%0 %public-keys (set ship)]]` and stays on that flow,
  so the moon's rekeys, breaches and sponsor changes reach us. The parent
  must be online.
- **Ping.** Every 25 seconds, the engine pokes its galaxy's `%ping` agent
  with `[%m %noun ~]`, one at a time, so the galaxy keeps a route to us.
  Off for galaxies and with `mesa_config.no_ping`.
- **Classic ames migration.** A ship that still sends us classic ames
  packets is sent an `%ahoy` plea over classic ames (at most one per 5
  seconds), which moves its flows with us to mesa. An `%ahoy` plea to us
  is acked; other `%$` pleas besides `%cork` are nacked.

## Helpers in the bindings

The Python and Go bindings carry the usual gall client code as helpers,
so an application doesn't write it into its handlers
([python.md](python.md#helpers), [go.md](go.md#helpers)):

- **Subs** keeps subscriptions alive. It watches on a fresh flow per
  subscription; on a kick it corks that flow (as gall does after a
  `[%x ~]`) and watches again on a new one, at once the first time and
  then with a growing backoff while kicks keep coming. A watch-nack ends
  the subscription for good. A leave is followed by a cork. Facts are
  routed by flow to the subscription they belong to.
- **Pokes** keeps one flow per `(ship, agent)`, so pokes arrive in the
  order sent, and numbers them so that each ack or nack can be matched
  to its poke (acks on a flow come in order).
- **Tlon DMs** (`tlon.py`, Go package `tlon`) speak the protocol below.

Their state (which flow is which, pending resubscribes, messages already
seen) is kept in the store beside the engine's, so it commits with each
ack and survives restarts. Both bindings use the same keys, so a
database works with either.

## Worked example: Tlon DMs

`bindings/python/tlon.py` and the Go package `bindings/go/tlon` speak
Tlon's DM protocol, as found in `tloncorp/tlon-apps`
(`desk/app/chat.hoon`); `bindings/python/examples/tlon_dms.py` (a web
page for your DMs) and `bindings/go/examples/dmbot` (a bot) use them.
That protocol is Tlon's, and can change upstream without notice. Check
the current `sur/` types if something stops working.

**The agent.** DMs go to agent `%chat` on the other ship, and come to
`%chat` on ours. So we serve `%chat` and poke `%chat`.

**Version check.** Before sending, Tlon watches
`/~/negotiate/version/chat-dms` on our `%chat`. We ack and give one fact,
mark `%noun`, noun `2`:

```python
def handle_watch(self, node, her, bone, agent, path):
    if agent != AGENT or path != VERSION_PATH:
        return None                      # not ours: the app's
    node.fact(her, bone, "noun", VERSION)
    return m.ACK
```

**Sending a DM.** One flow per conversation, so our messages arrive in
order (a `Pokes` helper keeps it, in the store). Each message is a poke
with mark `%chat-dm-diff-2`:

```
[id=[author=@p sent=@da] %add essay=[[content=story author=@p sent=@da] kind=[%chat ~] meta=~ blob=~] time=~]
```

that is, `[[author sent] [%add [[story author sent] [%chat ~] ~ ~] ~]]`.
`author` is our ship as an atom and `sent` a fresh `@da` that is the
message's id (`next_da()` keeps it unique). In the Python binding, tuples
are right-associated cells and `None` is `~`:

```python
sent = self.next_da()
us = atom_ship(node.our)
seq = self.pokes.poke(node, her, AGENT, MARK,
                      ((us, sent),
                       ("add", ((story_, us, sent), ("chat", None), None, None), None)))
```

The ack or nack comes back numbered by `seq`; the helper keeps each
pending message's id under its `seq`, and reports delivery by id
(`on_sent`).

**Stories.** `content` is a story: a list of verses. A verse is
`[%inline (list inline)]` or `[%block ...]`. An inline is a cord (plain
text) or a tagged cell; the ones `story_text()` reads are `[%break ~]`,
`[%bold (list inline)]`, `[%italics ...]`, `[%strike ...]`,
`[%blockquote ...]`, `[%inline-code cord]`, `[%code cord]`, `[%tag cord]`,
`[%ship @p]`, `[%link href text]`, `[%block p cord]` and
`[%task p (list inline)]`. For plain text, `story()` builds one paragraph
with line breaks as `%break`:

```
~[[%inline ~['first line' [%break ~] 'second line']]]
```

**Receiving a DM.** Tlon pokes our `%chat` with the same noun (the
helper accepts any `%chat-dm-diff` mark: `-2`, `-1` and unversioned,
reading them the same way). It takes the `%add` deltas: the essay's
`[content author sent]` is `delta.tail.head.head`, and `sent` is the
message id, used to ignore a message Tlon resends on a new flow (the
helper records each id it has seen). Other deltas (deletes, reactions,
replies) are acked and not shown. Other marks poked at `%chat` (RSVPs
and (un)blocks; Tlon's mark for the RSVP to a DM invite is
`%chat-dm-rsvp`) are acked and ignored. Pokes to any other agent are
left to the application.

Each received message is recorded, and handed to the application, in the
transaction that acks it, so a crash never loses or repeats one.

## Worked example: the guestbook

`examples/guestbook.c` is agent `%guestbook`, served entirely from
`hooks.plea` and `hooks.peek`, with its tables in the engine's SQLite
database (`mesa_store_sqlite_db()`).

- **Poke** with a cord: `:~our/guestbook 'hello!'` from a dojo (a `%noun`
  poke) or `mesa-ping ... ~our guestbook 'hello!'`. The noun must be a
  non-empty cord of at most 1024 bytes, or the poke is nacked with
  `[%bad-entry ~[leaf+"..."]]`. The guestbook doesn't check the mark. The
  insert, a fact to each subscriber and the ack commit together.
- **Watch** `/entries`: acked, then the latest 10 entries as facts, then
  one fact per new entry. Each fact is `%noun` `[id=@ud who=@p text=@t]`.
  Any other path is nacked with `%bad-path`. Subscribers are stored as
  `(ship, bone)`; a leave deletes the row, and so does a breach.
- **Remote scry** `/g/x/<n>/guestbook//1/entry`: entry *n* as `%noun`
  `[id who text]`. Entries never change, so the binding is immutable.
- Everything else, including pokes to other agents, is `MESA_DROP`.

```
mesa-ping ... ~our guestbook 'hello!'
mesa-ping ... -w /entries ~our guestbook
mesa-ping ... -x /g/x/3/guestbook//1/entry ~our
```

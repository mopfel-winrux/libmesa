# libmesa

A standalone C library for speaking Urbit's **mesa** networking protocol, so
any application can talk to Urbit ships as if it were one.

Status:

- **Step 1, wire core (done).** Packet codec, LSS (BLAKE3 Merkle streaming
  verification), bitsets, `@p` rendering/parsing and `+mug`, extracted from
  vere with no dependency on vere's noun library or event loop.
- **Step 2, nouns and crypto (done).** Standalone refcounted nouns with
  `jam`/`cue`/`mug`, and the mesa crypto layer (`++crypt` in ames.hoon):
  key derivation, XChaCha8, sealed paths, signatures and MACs. Both are
  checked byte-for-byte against Hoon.
- **Step 3, identities (done).** Keyfiles (`@uw` jammed `+feed`/`+seed`),
  fake-ship keys, comet mining (`+come:dawn`) and comet key checks, and
  the azimuth `getPoint`/`getDns` request builder and response parser.

## Layout

```
src/
  compat.h      shims for the u3 facilities the extracted code still names
  util.c        logging hook, assertions, mesa_mug_bytes (= u3r_mug_bytes)
  ship.{c,h}    u3_ship, @p render/parse (+fein/+fynd:ob), +sein
  noun.c        nouns: refcounting, jam/cue/mug/eq (iterative, no recursion)
  crypto.{c,h}  ames.hoon ++crypt: blake3 kdf/keyed, XChaCha8, seal-path,
                encrypt/decrypt, ed25519 sign/verify, slar, keyed MACs
  sha.{c,h}     +shay/+shax/+shal/+shas/+shaf
  ident.c       keyfiles, fake keys, comet mining/checking (mesa/ident.h)
  json.{c,h}    small JSON reader
  azimuth.c     azimuth-rpc getPoint/getDns (mesa/azimuth.h)
  arena.h       bump allocator (from vere)
  wire/
    mesa.h      packet structs (from vere/pkg/vere/io/mesa/mesa.h)
    pact.c      packet etch/sift (from vere/pkg/vere/io/mesa/pact.c)
    lss.{c,h}   LSS builder/verifier (from vere/pkg/vere/io/lss.c)
    bitset.{c,h}
include/mesa/   public headers
vendor/         c3 (from vere/pkg/c3), murmur3, verstable
test/           unit tests
```

The extracted files keep vere's naming and idioms on purpose, so changes can
flow back to vere.

## Build

Requires zig 0.15.2 (the same version vere uses).

```
zig build                      # zig-out/lib/{libmesa.a,libmesa-urcrypt.a}
zig build test                 # all unit tests
zig build -Doptimize=ReleaseFast
```

Only the BLAKE3, ChaCha (monocypher) and ed25519 parts of urcrypt are built.
That is everything mesa needs; there is no secp256k1, nettle or argon2.

## Test vectors

`test/noun_test.c`, `test/crypto_test.c` and `test/ident_test.c` embed
values computed by Hoon
(`urbit eval`). The crypto vectors paste the ames `++crypt` arms verbatim.
The ivory kernel available predates `+luck`/`+slar`/`+sign-octs-raw`, so the
vectors use `+puck`/`+shar`/`+sign`, which compute the same values from seeds.

## Deviations from vere

- `pact.c`: `log_name()` renders `@p` in C, not via Hoon `scot`. The
  `PACT_TEST` section no longer boots the ivory pill; the Hoon `stab` path
  comparison was dropped.
- `lss.c`: shifts by a variable height are now unsigned. `1<<31` was
  undefined behaviour in `lss_builder_finalize` (found by UBSan in the
  test). The `LSS_TEST` section was updated for the arena argument of
  `lss_verifier_init`.
- `bitset.c`: the `BITSET_TEST` section was updated for the arena argument
  of `bitset_init`.
- `noun.c` (new, replaces u3 nouns): jam's dedup table hashes with a 64-bit
  structural hash rather than `+mug`. Iterating the 31-bit `+mug` up a deep
  spine cycles after ~2^15.5 levels, which made jam quadratic on deep
  left-nested nouns. jam's output doesn't depend on the hash.

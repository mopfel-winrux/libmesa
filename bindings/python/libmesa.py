"""libmesa from Python: talk to Urbit ships as one.

A ctypes binding to libmesa.so (`zig build` makes zig-out/lib/libmesa.so;
or set $LIBMESA). The engine is sans-IO, so this module drives it with
Python's own sockets and timers, resolves keys over HTTPS with urllib, and
keeps libmesa's state in a SQLite database that the application can put
its own tables in: handlers run inside the same transaction, so each
message is processed exactly once.

    import libmesa

    class Echo(libmesa.Node):
        def on_poke(self, her, agent, mark, noun):
            print(libmesa.ship_text(her), "says", libmesa.cord(noun))
            return True                     # ack; False or raise to nack

    node = Echo(libmesa.Identity.comet("echo.key"), "echo.db")
    node.run()

Nouns are plain Python values on the way in (int, str for cords, bytes,
tuples for cells, None for ~) and `Noun` handles on the way out.
"""

import ctypes as C
import fcntl
import os
import queue
import select
import socket
import sqlite3
import sys
import threading
import time
import traceback
import urllib.request

# ---------------------------------------------------------------- loading

def _load():
    here = os.path.dirname(os.path.abspath(__file__))
    for path in (os.environ.get("LIBMESA"),
                 os.path.join(here, "..", "..", "zig-out", "lib", "libmesa.so"),
                 "libmesa.so"):
        if path and (os.path.exists(path) or "/" not in path):
            try:
                return C.CDLL(path)
            except OSError:
                pass
    raise OSError("can't find libmesa.so: run `zig build` or set $LIBMESA")

lib = _load()

# ------------------------------------------------------------------ types

noun_t = C.c_uint64
NONE = (1 << 64) - 1                 # MESA_NONE

class Ship(C.Structure):
    _fields_ = [("lo", C.c_uint64), ("hi", C.c_uint64)]

    def __str__(self):
        return ship_text(self)

    def __repr__(self):
        return "Ship(%s)" % self

    def __eq__(self, other):
        return (self.lo, self.hi) == (other.lo, other.hi)

    def __hash__(self):
        return hash((self.lo, self.hi))

class Ident(C.Structure):
    _fields_ = [("who", Ship), ("rift", C.c_uint64), ("life", C.c_uint64),
                ("ring", C.c_uint8 * 65)]

class Point(C.Structure):
    _fields_ = [("who", Ship), ("rift", C.c_uint64), ("life", C.c_uint64),
                ("pass_", C.c_uint8 * 65), ("has_sponsor", C.c_bool),
                ("sponsor", Ship)]

LANE_NONE, LANE_GALAXY, LANE_IP = 0, 1, 2

class Lane(C.Structure):
    _fields_ = [("kind", C.c_int), ("galaxy", C.c_uint8),
                ("ip", C.c_uint32), ("port", C.c_uint16)]

SCAN_F = C.CFUNCTYPE(C.c_int, C.c_void_p, C.POINTER(C.c_uint8), C.c_size_t,
                     C.POINTER(C.c_uint8), C.c_size_t)

class Store(C.Structure):
    _fields_ = [
        ("ctx", C.c_void_p),
        ("begin", C.CFUNCTYPE(C.c_int, C.c_void_p)),
        ("commit", C.CFUNCTYPE(C.c_int, C.c_void_p)),
        ("rollback", C.CFUNCTYPE(C.c_int, C.c_void_p)),
        ("mark", C.CFUNCTYPE(C.c_int, C.c_void_p)),
        ("rewind", C.CFUNCTYPE(C.c_int, C.c_void_p)),
        ("release", C.CFUNCTYPE(C.c_int, C.c_void_p)),
        ("get", C.CFUNCTYPE(C.c_int, C.c_void_p, C.POINTER(C.c_uint8), C.c_size_t,
                            C.POINTER(C.POINTER(C.c_uint8)), C.POINTER(C.c_size_t))),
        ("put", C.CFUNCTYPE(C.c_int, C.c_void_p, C.POINTER(C.c_uint8), C.c_size_t,
                            C.POINTER(C.c_uint8), C.c_size_t)),
        ("del_", C.CFUNCTYPE(C.c_int, C.c_void_p, C.POINTER(C.c_uint8), C.c_size_t)),
        ("scan", C.CFUNCTYPE(C.c_int, C.c_void_p, C.POINTER(C.c_uint8), C.c_size_t,
                             SCAN_F, C.c_void_p)),
        ("close", C.CFUNCTYPE(None, C.c_void_p)),
    ]

ACK, NACK, LATER, DROP = 0, 1, 2, 3

class Hooks(C.Structure):
    _fields_ = [
        ("ctx", C.c_void_p),
        ("send", C.CFUNCTYPE(None, C.c_void_p, Lane, C.POINTER(C.c_uint8), C.c_size_t)),
        ("need_point", C.CFUNCTYPE(None, C.c_void_p, Ship)),
        ("plea", C.CFUNCTYPE(C.c_int, C.c_void_p, Ship, C.c_uint64, noun_t, noun_t,
                             noun_t, C.POINTER(noun_t))),
        ("boon", C.CFUNCTYPE(None, C.c_void_p, Ship, C.c_uint64, noun_t)),
        ("done", C.CFUNCTYPE(None, C.c_void_p, Ship, C.c_uint64, noun_t)),
        ("corked", C.CFUNCTYPE(None, C.c_void_p, Ship, C.c_uint64)),
        ("sage", C.CFUNCTYPE(None, C.c_void_p, Ship, C.c_char_p, noun_t)),
        ("peek", C.CFUNCTYPE(noun_t, C.c_void_p, C.POINTER(Ship), C.c_char_p)),
        ("breach", C.CFUNCTYPE(None, C.c_void_p, Ship)),
    ]

class Config(C.Structure):
    _fields_ = [("ident", Ident), ("store", C.POINTER(Store)), ("hooks", Hooks),
                ("our_sponsor", Ship), ("breached", C.c_bool),
                ("no_ping", C.c_bool), ("fake", C.c_bool), ("now", C.c_uint64)]

GALL_NONE, GALL_POKE, GALL_WATCH, GALL_WATCH_AS, GALL_LEAVE, GALL_SYSTEM = range(6)
GALL_BAD, GALL_FACT, GALL_KICK = range(3)

class GallRequest(C.Structure):
    _fields_ = [("kind", C.c_int), ("agent", C.c_char * 64), ("mark", noun_t),
                ("noun", noun_t), ("path", noun_t)]

AZ_OK, AZ_NOT_FOUND, AZ_BAD = 0, 1, -1

def _sig(name, res, *args):
    f = getattr(lib, name)
    f.restype = res
    f.argtypes = list(args)
    return f

E = C.c_void_p       # mesa_engine*
_sig("mesa_malloc", C.c_void_p, C.c_size_t)
_sig("mesa_free", None, C.c_void_p)
_sig("mesa_ref", noun_t, noun_t)
_sig("mesa_unref", None, noun_t)
_sig("mesa_atom_u64", noun_t, C.c_uint64)
_sig("mesa_atom_bytes", noun_t, C.c_size_t, C.c_char_p)
_sig("mesa_is_atom", C.c_bool, noun_t)
_sig("mesa_is_cell", C.c_bool, noun_t)
_sig("mesa_met3", C.c_size_t, noun_t)
_sig("mesa_atom_get_bytes", None, noun_t, C.c_size_t, C.c_char_p)
_sig("mesa_cell", noun_t, noun_t, noun_t)
_sig("mesa_head", noun_t, noun_t)
_sig("mesa_tail", noun_t, noun_t)
_sig("mesa_cue_bytes", noun_t, C.c_size_t, C.c_char_p)
_sig("mesa_ship_parse", C.c_bool, C.c_char_p, C.POINTER(Ship))
_sig("mesa_ship_render", C.c_void_p, Ship)
_sig("mesa_ship_rank", C.c_int, Ship)
_sig("mesa_ident_fake", None, Ship, C.POINTER(Ident))
_sig("mesa_ident_pass", None, C.POINTER(Ident), C.c_char_p)
_sig("mesa_keyfile_parse", C.c_int, C.c_char_p, C.POINTER(Ident), C.c_size_t)
_sig("mesa_keyfile_render", C.c_void_p, C.POINTER(Ident))
_sig("mesa_comet_mine", C.c_bool, C.c_size_t, C.POINTER(Ship), C.c_char_p,
     C.c_uint64, C.POINTER(Ident))
_sig("mesa_azimuth_point_request", C.c_void_p, Ship)
_sig("mesa_azimuth_point_parse", C.c_int, Ship, C.c_size_t, C.c_char_p, C.POINTER(Point))
_sig("mesa_engine_open", C.c_int, C.POINTER(Config), C.POINTER(E))
_sig("mesa_engine_close", None, E)
_sig("mesa_engine_hear", None, E, Lane, C.c_char_p, C.c_size_t, C.c_uint64)
_sig("mesa_engine_wake", None, E, C.c_uint64)
_sig("mesa_engine_deadline", C.c_uint64, E)
_sig("mesa_engine_point", None, E, C.POINTER(Point))
_sig("mesa_engine_point_missing", None, E, Ship)
_sig("mesa_txn_begin", C.c_int, E)
_sig("mesa_txn_commit", C.c_int, E)
_sig("mesa_txn_abort", None, E)
_sig("mesa_flow_open", C.c_int, E, Ship, C.POINTER(C.c_uint64))
_sig("mesa_flow_cork", C.c_int, E, Ship, C.c_uint64)
_sig("mesa_keen", C.c_int, E, Ship, C.c_char_p)
_sig("mesa_gall_poke", C.c_int, E, Ship, C.c_uint64, C.c_char_p, C.c_char_p, noun_t)
_sig("mesa_gall_watch", C.c_int, E, Ship, C.c_uint64, C.c_char_p, noun_t)
_sig("mesa_gall_leave", C.c_int, E, Ship, C.c_uint64, C.c_char_p)
_sig("mesa_gall_fact", C.c_int, E, Ship, C.c_uint64, C.c_char_p, noun_t)
_sig("mesa_gall_response", C.c_int, noun_t, C.POINTER(noun_t), C.POINTER(noun_t))
_sig("mesa_gall_request_parse", C.c_int, noun_t, noun_t, noun_t, C.POINTER(GallRequest))

MESA_AZIMUTH_URL = "https://roller.urbit.org/v1/azimuth"
RANK_CZAR, RANK_KING, RANK_DUKE, RANK_EARL, RANK_PAWN = range(5)

class MesaError(Exception):
    pass

def _check(err, what):
    if err != 0:
        raise MesaError("%s: error %d" % (what, err))

def _take_str(ptr):
    """a malloc'd C string from libmesa, as Python str"""
    if not ptr:
        return None
    try:
        return C.string_at(ptr).decode("utf-8", "replace")
    finally:
        lib.mesa_free(ptr)

# ------------------------------------------------------------------ ships

def ship(text):
    """Ship from "~sampel-palnet" """
    who = Ship()
    if not lib.mesa_ship_parse(text.encode(), C.byref(who)):
        raise ValueError("not a ship: %r" % text)
    return who

def ship_text(who):
    return _take_str(lib.mesa_ship_render(who))

# ------------------------------------------------------------------ nouns

def noun(x):
    """a new noun (an owned reference) from a Python value:
    int, str (a cord), bytes (an atom, little-endian), None (~), or a
    tuple/list of two or more, right-associated into cells."""
    if x is None:
        return lib.mesa_atom_u64(0)
    if isinstance(x, bool):
        return lib.mesa_atom_u64(0 if x else 1)          # loobean
    if isinstance(x, int):
        if x < 0:
            raise ValueError("atoms are unsigned")
        if x < (1 << 64):
            return lib.mesa_atom_u64(x)
        b = x.to_bytes((x.bit_length() + 7) // 8, "little")
        return lib.mesa_atom_bytes(len(b), b)
    if isinstance(x, str):
        b = x.encode("utf-8")
        return lib.mesa_atom_bytes(len(b), b)
    if isinstance(x, (bytes, bytearray)):
        return lib.mesa_atom_bytes(len(x), bytes(x))
    if isinstance(x, Noun):
        return lib.mesa_ref(x.n)
    if isinstance(x, (tuple, list)):
        if len(x) < 2:
            raise ValueError("a cell needs two or more items")
        n = noun(x[-1])
        for i in reversed(x[:-1]):
            n = lib.mesa_cell(noun(i), n)
        return n
    raise TypeError("can't make a noun of %r" % (x,))

DA_UNIX_EPOCH = 0x8000000cce9e0d80        # ~1970.1.1, in seconds

def da(unix=None):
    """an @da (Urbit date) from Unix seconds (default: now)"""
    t = time.time() if unix is None else unix
    sec = int(t)
    frac = int((t - sec) * (1 << 64)) & ((1 << 64) - 1)
    return ((DA_UNIX_EPOCH + sec) << 64) | frac

def unix(d):
    """Unix seconds from an @da"""
    return ((d >> 64) - DA_UNIX_EPOCH) + (d & ((1 << 64) - 1)) / (1 << 64)

def path(text):
    """a path noun from "/a/b/c" """
    segs = [s for s in text.split("/") if s]
    return tuple(segs) + (None,) if segs else None

class Noun:
    """a borrowed noun handle, valid during the hook that handed it out"""

    __slots__ = ("n",)

    def __init__(self, n):
        self.n = n

    @property
    def is_cell(self):
        return bool(lib.mesa_is_cell(self.n))

    @property
    def head(self):
        return Noun(lib.mesa_head(self.n))

    @property
    def tail(self):
        return Noun(lib.mesa_tail(self.n))

    def __int__(self):
        if self.is_cell:
            raise TypeError("a cell, not an atom")
        size = lib.mesa_met3(self.n)
        buf = C.create_string_buffer(size)
        lib.mesa_atom_get_bytes(self.n, size, buf)
        return int.from_bytes(buf.raw, "little")

    def bytes(self):
        size = lib.mesa_met3(self.n)
        buf = C.create_string_buffer(size)
        lib.mesa_atom_get_bytes(self.n, size, buf)
        return buf.raw

    def text(self):
        """an atom as a cord"""
        return self.bytes().decode("utf-8", "replace")

    def py(self, depth=0):
        """the whole noun as Python: ints, and (head, tail) tuples"""
        if depth > 1000:
            raise ValueError("too deep")
        if self.is_cell:
            return (self.head.py(depth + 1), self.tail.py(depth + 1))
        return int(self)

    def __repr__(self):
        return "Noun(%r)" % (self.py(),)

def cord(n):
    """text of a cord noun (Noun or raw handle), or None if it's a cell"""
    n = n if isinstance(n, Noun) else Noun(n)
    return None if n.is_cell else n.text()

def tang(err):
    """lines of an error noun [tag tang] (a nack's naxplanation)"""
    lines = []

    def tank(t, depth=0):
        if depth > 32 or not t.is_cell or t.head.is_cell:
            return ""
        tag = t.head.text()
        if tag == "leaf":
            return tape(t.tail)
        if tag in ("rose", "palm"):
            pre = t.tail.head
            sep, opn, cls = (pre.head, pre.tail.head, pre.tail.tail)
            if tag == "palm":
                sep, cls = pre.tail.tail.head, pre.tail.tail.tail.tail
            parts, kids = [], t.tail.tail
            while kids.is_cell:
                parts.append(tank(kids.head, depth + 1))
                kids = kids.tail
            return tape(opn) + tape(sep).join(parts) + tape(cls)
        return ""

    def tape(t):
        out = bytearray()
        while t.is_cell:
            out.append(int(t.head) & 0xff)
            t = t.tail
        return out.decode("utf-8", "replace")

    if err.is_cell:
        t = err.tail
        while t.is_cell:
            lines.append(tank(t.head))
            t = t.tail
    return lines

# -------------------------------------------------------------- identities

class Identity:
    """who we are: a fake ship, a keyfile, or a comet"""

    def __init__(self, ident, fake=False):
        self.ident = ident
        self.fake = fake

    @property
    def ship(self):
        return Ship(self.ident.who.lo, self.ident.who.hi)

    @classmethod
    def fake(cls, who):
        """a fake ship (`urbit -F`), for local testing"""
        ide = Ident()
        lib.mesa_ident_fake(ship(who) if isinstance(who, str) else who, C.byref(ide))
        return cls(ide, fake=True)

    @classmethod
    def keyfile(cls, path_):
        ide = Ident()
        with open(path_) as f:
            if lib.mesa_keyfile_parse(f.read().strip().encode(), C.byref(ide), 1) < 1:
                raise ValueError("bad keyfile %s" % path_)
        return cls(ide)

    @classmethod
    def comet(cls, path_):
        """the comet whose keyfile is at [path_], mining one if there's none"""
        if os.path.exists(path_):
            return cls.keyfile(path_)
        with urllib.request.urlopen("https://bootstrap.urbit.org/comet-stars.jam",
                                    timeout=30) as r:
            jam = r.read()
        lis = lib.mesa_cue_bytes(len(jam), jam)
        if lis == NONE:
            raise MesaError("bad comet-stars.jam")
        stars, n = [], Noun(lis)
        while n.is_cell:
            s = int(n.head)
            stars.append(Ship(s, 0))
            n = n.tail
        lib.mesa_unref(lis)
        arr = (Ship * len(stars))(*stars)
        ide = Ident()
        print("mining a comet under %d stars..." % len(stars), file=sys.stderr)
        if not lib.mesa_comet_mine(len(stars), arr, os.urandom(64), (1 << 64) - 1,
                                   C.byref(ide)):
            raise MesaError("mining failed")
        with open(path_, "w") as f:
            f.write(_take_str(lib.mesa_keyfile_render(C.byref(ide))) + "\n")
        return cls(ide)

# ----------------------------------------------------------------- the store

class SqliteStore:
    """libmesa's state in a Python sqlite3 connection, so the application's
    own tables (in self.db) commit in the same transactions"""

    def __init__(self, path_):
        self.lock = open(path_ + ".lock", "w")
        try:
            fcntl.flock(self.lock, fcntl.LOCK_EX | fcntl.LOCK_NB)
        except OSError:
            raise MesaError("%s is in use by another process" % path_)
        self.db = sqlite3.connect(path_, isolation_level=None, check_same_thread=False)
        self.db.execute("PRAGMA journal_mode=WAL")
        self.db.execute("PRAGMA synchronous=FULL")
        self.db.execute("CREATE TABLE IF NOT EXISTS mesa_kv "
                        "(k BLOB PRIMARY KEY, v BLOB NOT NULL) WITHOUT ROWID")
        self.depth = 0
        self.fresh = self.db.execute("SELECT count(*) FROM mesa_kv").fetchone()[0] == 0

        def guard(fn):
            def call(*a):
                try:
                    return fn(*a)
                except Exception:
                    traceback.print_exc()
                    return -1
            return call

        def kb(p, n):
            return C.string_at(p, n)

        def get(ctx, k, kn, out, outn):
            row = self.db.execute("SELECT v FROM mesa_kv WHERE k = ?",
                                  (kb(k, kn),)).fetchone()
            if row is None:
                return 1
            v = bytes(row[0])
            buf = lib.mesa_malloc(len(v))
            C.memmove(buf, v, len(v))
            out[0] = C.cast(buf, C.POINTER(C.c_uint8))
            outn[0] = len(v)
            return 0

        def put(ctx, k, kn, v, vn):
            self.db.execute("INSERT OR REPLACE INTO mesa_kv VALUES (?, ?)",
                            (kb(k, kn), kb(v, vn)))
            return 0

        def delete(ctx, k, kn):
            self.db.execute("DELETE FROM mesa_kv WHERE k = ?", (kb(k, kn),))
            return 0

        def scan(ctx, pre, pn, fun, arg):
            p = kb(pre, pn)
            rows = self.db.execute("SELECT k, v FROM mesa_kv WHERE substr(k, 1, ?) = ? "
                                   "ORDER BY k", (len(p), p)).fetchall()
            for k, v in rows:
                k, v = bytes(k), bytes(v)
                kp = (C.c_uint8 * max(1, len(k))).from_buffer_copy(k or b"\0")
                vp = (C.c_uint8 * max(1, len(v))).from_buffer_copy(v or b"\0")
                if fun(arg, kp, len(k), vp, len(v)):
                    return 1
            return 0

        def ex(sql):
            def f(ctx):
                self.db.execute(sql)
                return 0
            return f

        def mark(ctx):
            self.depth += 1
            self.db.execute("SAVEPOINT m%d" % self.depth)
            return 0

        def rewind(ctx):
            self.db.execute("ROLLBACK TO m%d" % self.depth)
            return 0

        def release(ctx):
            self.db.execute("RELEASE m%d" % self.depth)
            self.depth -= 1
            return 0

        def rollback(ctx):
            self.depth = 0
            self.db.execute("ROLLBACK")
            return 0

        f = dict(Store._fields_)
        self.c = Store(
            None,
            f["begin"](guard(ex("BEGIN IMMEDIATE"))),
            f["commit"](guard(ex("COMMIT"))),
            f["rollback"](guard(rollback)),
            f["mark"](guard(mark)),
            f["rewind"](guard(rewind)),
            f["release"](guard(release)),
            f["get"](guard(get)),
            f["put"](guard(put)),
            f["del_"](guard(delete)),
            f["scan"](guard(scan)),
            f["close"](lambda ctx: None),
        )

# ------------------------------------------------------------------ the node

class Node:
    """A ship on the network. Subclass it and override the on_* methods.

    Handlers run inside the event's transaction: write your tables in
    self.db and they commit together with the ack. Send messages from a
    handler, or anywhere else inside `with node.txn():`.
    """

    def __init__(self, identity, db_path, breached=False, port=0, fake=None):
        self.identity = identity
        self.fake = identity.fake if fake is None else fake
        self.store = SqliteStore(db_path)
        self.db = self.store.db
        self.our = identity.ship
        self.galaxies = {}           # galaxy -> (ip, port, when)
        self.points = queue.Queue()  # (ship, Point or None) from resolver threads
        self.asking = set()
        self.running = False
        self.in_hook = False
        self.trace = bool(os.environ.get("MESA_TRACE"))

        self.sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
        if lib.mesa_ship_rank(self.our) == RANK_CZAR and not port:
            port = (31337 if self.fake else 13337) + self.our.lo
        self.sock.bind(("127.0.0.1" if self.fake else "0.0.0.0", port))
        self.sock.setblocking(False)

        self.setup()

        # our own point: our sponsor, and a check of our keys
        sponsor = Ship()
        if not self.fake and lib.mesa_ship_rank(self.our) in (RANK_KING, RANK_DUKE):
            pot = self._resolve(self.our)
            if pot is None:
                raise MesaError("can't find %s on azimuth" % self.our)
            ours = (C.c_uint8 * 65)()
            lib.mesa_ident_pass(C.byref(identity.ident), C.cast(ours, C.c_char_p))
            if bytes(pot.pass_) != bytes(ours) or pot.life != identity.ident.life:
                raise MesaError("our keys don't match azimuth")
            if identity.ident.rift == NONE:
                identity.ident.rift = pot.rift
            if pot.has_sponsor:
                sponsor = pot.sponsor
        if identity.ident.rift == NONE:
            identity.ident.rift = 0

        h = dict(Hooks._fields_)
        self._hooks = Hooks(
            None,
            h["send"](self._send),
            h["need_point"](self._need_point),
            h["plea"](self._plea),
            h["boon"](self._boon),
            h["done"](self._done),
            h["corked"](self._corked),
            h["sage"](self._sage),
            h["peek"](self._peek),
            h["breach"](self._breach),
        )
        #  comets and fake ships may always start fresh; anyone else only
        #  if they say they've breached
        cfg = Config(identity.ident, C.pointer(self.store.c), self._hooks, sponsor,
                     breached or self.fake, False, self.fake, self._now())
        self.eng = E()
        err = lib.mesa_engine_open(C.byref(cfg), C.byref(self.eng))
        if err == -2:
            raise MesaError("this identity has run before, but its state is "
                            "empty: pass breached=True only if it has breached")
        _check(err, "mesa_engine_open")

    # ------------------------------------------------- to override

    def setup(self):
        """create your tables in self.db (runs once, before the engine opens)"""

    def on_poke(self, her, agent, mark, noun_):
        """a poke to [agent]; return True to ack, False (or raise) to nack,
        or DROP for agents you don't run: no answer, so they resend"""
        return DROP

    def on_watch(self, her, bone, agent, path_):
        """a subscription; return True to accept (then give facts with
        fact()), False to refuse, or DROP"""
        return DROP

    def on_leave(self, her, bone, agent):
        pass

    def on_plea(self, her, bone, vane, path_, payload):
        """any other request; return ACK, NACK, LATER or DROP"""
        return DROP

    def on_fact(self, her, bone, mark, noun_):
        """a fact on a subscription of ours"""

    def on_boon(self, her, bone, payload):
        """any other response on a flow of ours"""

    def on_done(self, her, bone, error):
        """our message on [bone] was acked (error None) or nacked (error: list
        of lines)"""

    def on_scry(self, her, path_, mark, noun_):
        """the answer to scry() (mark None if cancelled)"""

    def on_peek(self, her, path_):
        """serve our namespace: return (mark, value) or None"""
        return None

    def on_breach(self, her):
        pass

    def on_tick(self):
        """called every loop iteration (outside any transaction)"""

    # ------------------------------------------------- sending

    def open_flow(self, her):
        bone = C.c_uint64()
        _check(lib.mesa_flow_open(self.eng, her, C.byref(bone)), "mesa_flow_open")
        return bone.value

    def poke(self, her, bone, agent, mark, value):
        _check(lib.mesa_gall_poke(self.eng, her, bone, agent.encode(), mark.encode(),
                                  noun(value)), "mesa_gall_poke")

    def watch(self, her, bone, agent, path_):
        _check(lib.mesa_gall_watch(self.eng, her, bone, agent.encode(), noun(path(path_))),
               "mesa_gall_watch")

    def leave(self, her, bone, agent):
        _check(lib.mesa_gall_leave(self.eng, her, bone, agent.encode()), "mesa_gall_leave")

    def fact(self, her, bone, mark, value):
        _check(lib.mesa_gall_fact(self.eng, her, bone, mark.encode(), noun(value)),
               "mesa_gall_fact")

    def cork(self, her, bone):
        _check(lib.mesa_flow_cork(self.eng, her, bone), "mesa_flow_cork")

    def scry(self, her, path_):
        _check(lib.mesa_keen(self.eng, her, path_.encode()), "mesa_keen")

    def txn(self):
        """`with node.txn():` group your writes and sends into one commit"""
        node = self

        class _Txn:
            def __enter__(self):
                if node.in_hook:
                    return self          # hooks already run in one
                _check(lib.mesa_txn_begin(node.eng), "mesa_txn_begin")
                self.mine = True
                return self

            def __exit__(self, typ, val, tb):
                if getattr(self, "mine", False):
                    if typ is None:
                        _check(lib.mesa_txn_commit(node.eng), "mesa_txn_commit")
                    else:
                        lib.mesa_txn_abort(node.eng)
                return False

        return _Txn()

    # ------------------------------------------------- hooks (from C)

    def _guard(self, fn, *a, default=None):
        self.in_hook = True
        try:
            return fn(*a)
        except Exception:
            traceback.print_exc()
            return default
        finally:
            self.in_hook = False

    def _send(self, ctx, lane, buf, n):
        data = C.string_at(buf, n)
        addr = None
        if lane.kind == LANE_GALAXY:
            addr = self._galaxy(lane.galaxy)
        elif lane.kind == LANE_IP and lane.ip and lane.port:
            ip = "127.0.0.1" if self.fake else socket.inet_ntoa(lane.ip.to_bytes(4, "big"))
            addr = (ip, lane.port)
        if self.trace:
            print("mesa: send %d to %s" % (n, "%s:%d" % addr if addr else "nowhere"),
                  file=sys.stderr, flush=True)
        if addr:
            try:
                self.sock.sendto(data, addr)
            except OSError:
                pass                 # the engine resends

    def _galaxy(self, g):
        if self.fake:
            return ("127.0.0.1", 31337 + g)
        hit = self.galaxies.get(g)
        if hit and time.time() - hit[2] < 300:
            return hit[:2]
        name = ship_text(Ship(g, 0))[1:] + ".urbit.org"
        try:
            ip = socket.gethostbyname(name)
        except OSError:
            return hit[:2] if hit else None
        self.galaxies[g] = (ip, 13337 + g, time.time())
        return (ip, 13337 + g)

    def _resolve(self, who):
        """getPoint from azimuth (blocking); a Point or None"""
        body = _take_str(lib.mesa_azimuth_point_request(who)).encode()
        req = urllib.request.Request(MESA_AZIMUTH_URL, body,
                                     {"Content-Type": "application/json"})
        try:
            with urllib.request.urlopen(req, timeout=30) as r:
                js = r.read()
        except OSError:
            return None
        pot = Point()
        if lib.mesa_azimuth_point_parse(who, len(js), js, C.byref(pot)) != AZ_OK:
            return None
        return pot

    def _need_point(self, ctx, who):
        who = Ship(who.lo, who.hi)
        if self.fake:
            ide = Ident()
            lib.mesa_ident_fake(who, C.byref(ide))
            pot = Point(who, 0, 1)
            lib.mesa_ident_pass(C.byref(ide), C.cast(pot.pass_, C.c_char_p))
            pot.has_sponsor = False
            self.points.put((who, pot))
            return
        if who in self.asking:
            return
        self.asking.add(who)
        threading.Thread(target=lambda: self.points.put((who, self._resolve(who))),
                         daemon=True).start()

    def _plea(self, ctx, her, bone, vane, path_, payload, error):
        her = Ship(her.lo, her.hi)

        def run():
            req = GallRequest()
            kind = lib.mesa_gall_request_parse(vane, path_, payload, C.byref(req))
            agent = req.agent.decode()
            if kind == GALL_SYSTEM:
                return ACK
            if kind == GALL_POKE:
                return _answer(self.on_poke(her, agent, Noun(req.mark).text(),
                                            Noun(req.noun)))
            if kind in (GALL_WATCH, GALL_WATCH_AS):
                return _answer(self.on_watch(her, bone, agent,
                                             _path_text(Noun(req.path))))
            if kind == GALL_LEAVE:
                self.on_leave(her, bone, agent)
                return ACK
            return self.on_plea(her, bone, Noun(vane), Noun(path_), Noun(payload))

        self.in_hook = True
        try:
            return run()
        except Exception as e:
            traceback.print_exc()
            error[0] = noun(("python-error", ("leaf", *[ord(c) for c in str(e)[:200]], None),
                             None))
            return NACK
        finally:
            self.in_hook = False

    def _boon(self, ctx, her, bone, payload):
        her = Ship(her.lo, her.hi)
        mark, val = noun_t(), noun_t()
        kind = lib.mesa_gall_response(payload, C.byref(mark), C.byref(val))
        if kind == GALL_FACT:
            self._guard(self.on_fact, her, bone, Noun(mark.value).text(), Noun(val.value))
        else:
            self._guard(self.on_boon, her, bone, Noun(payload))

    def _done(self, ctx, her, bone, error):
        her = Ship(her.lo, her.hi)
        err = None if error == NONE else tang(Noun(error))
        self._guard(self.on_done, her, bone, err)

    def _corked(self, ctx, her, bone):
        pass

    def _sage(self, ctx, her, path_, gage):
        her = Ship(her.lo, her.hi)
        p = path_.decode()
        if gage == NONE:
            self._guard(self.on_scry, her, p, None, None)
        else:
            g = Noun(gage)
            self._guard(self.on_scry, her, p, g.head.text(), g.tail)

    def _peek(self, ctx, her, path_):
        who = Ship(her[0].lo, her[0].hi) if her else None
        got = self._guard(self.on_peek, who, path_.decode())
        if not got:
            return NONE
        return noun(got)

    def _breach(self, ctx, her):
        self._guard(self.on_breach, Ship(her.lo, her.hi))

    # ------------------------------------------------- the loop

    @staticmethod
    def _now():
        return int(time.time() * 1000)

    def step(self, timeout=0.1):
        """one turn of the loop: datagrams, keys, timers"""
        dea = lib.mesa_engine_deadline(self.eng)
        wait = timeout if dea == NONE else max(0, min(timeout, (dea - self._now()) / 1000))
        rd, _, _ = select.select([self.sock] + self.extra_fds(), [], [], wait)
        if self.sock in rd:
            for _ in range(256):
                try:
                    data, (ip, port) = self.sock.recvfrom(2048)
                except BlockingIOError:
                    break
                lane = Lane(LANE_IP, 0, int.from_bytes(socket.inet_aton(ip), "big"), port)
                if self.trace:
                    print("mesa: hear %d from %s:%d" % (len(data), ip, port),
                          file=sys.stderr, flush=True)
                lib.mesa_engine_hear(self.eng, lane, data, len(data), self._now())
        for fd in rd:
            if fd is not self.sock:
                self.on_fd(fd)
        while not self.points.empty():
            who, pot = self.points.get()
            self.asking.discard(who)
            if pot is None:
                lib.mesa_engine_point_missing(self.eng, who)
            else:
                lib.mesa_engine_point(self.eng, C.byref(pot))
        if lib.mesa_engine_deadline(self.eng) <= self._now():
            lib.mesa_engine_wake(self.eng, self._now())
        self.on_tick()

    def extra_fds(self):
        """more file descriptors for the loop to wait on (see on_fd)"""
        return []

    def on_fd(self, fd):
        pass

    def run(self):
        self.running = True
        try:
            while self.running:
                self.step()
        except KeyboardInterrupt:
            pass
        finally:
            self.close()

    def stop(self):
        self.running = False

    def close(self):
        if self.eng:
            lib.mesa_engine_close(self.eng)
            self.eng = None
            self.db.close()

def _answer(res):
    if res is DROP or (type(res) is int and res == DROP):
        return DROP
    return ACK if res else NACK

def _path_text(p):
    segs = []
    while p.is_cell:
        segs.append(p.head.text())
        p = p.tail
    return "/" + "/".join(segs)

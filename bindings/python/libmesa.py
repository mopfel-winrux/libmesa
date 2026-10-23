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
import heapq
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
        if not isinstance(other, Ship):
            return NotImplemented
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
_sig("mesa_flow_plea", C.c_int, E, Ship, C.c_uint64, noun_t, noun_t, noun_t)
_sig("mesa_flow_boon", C.c_int, E, Ship, C.c_uint64, noun_t)
_sig("mesa_flow_done", C.c_int, E, Ship, C.c_uint64, noun_t)
_sig("mesa_engine_peer", C.c_bool, E, Ship, C.POINTER(Point))
_sig("mesa_keen", C.c_int, E, Ship, C.c_char_p)
_sig("mesa_yawn", C.c_int, E, Ship, C.c_char_p)
_sig("mesa_gall_poke", C.c_int, E, Ship, C.c_uint64, C.c_char_p, C.c_char_p, noun_t)
_sig("mesa_gall_watch", C.c_int, E, Ship, C.c_uint64, C.c_char_p, noun_t)
_sig("mesa_gall_leave", C.c_int, E, Ship, C.c_uint64, C.c_char_p)
_sig("mesa_gall_fact", C.c_int, E, Ship, C.c_uint64, C.c_char_p, noun_t)
_sig("mesa_gall_kick", C.c_int, E, Ship, C.c_uint64)
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
            if tag == "rose":           # [p=sep q=open r=close]
                sep, opn, cls = (tape(pre.head), tape(pre.tail.head),
                                 tape(pre.tail.tail))
            else:                       # [p=sep q=open r=open s=close]
                sep = tape(pre.head)
                opn = tape(pre.tail.head) + tape(pre.tail.tail.head)
                cls = tape(pre.tail.tail.tail)
            parts, kids = [], t.tail.tail
            while kids.is_cell:
                parts.append(tank(kids.head, depth + 1))
                kids = kids.tail
            return opn + sep.join(parts) + cls
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
        with os.fdopen(os.open(path_, os.O_WRONLY | os.O_CREAT | os.O_EXCL, 0o600),
                       "w") as f:
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

    def __init__(self, identity, db_path, breached=False, port=0, fake=None, helpers=()):
        self.identity = identity
        self.fake = identity.fake if fake is None else fake
        self.store = SqliteStore(db_path)
        self.db = self.store.db
        self.our = identity.ship
        self.galaxies = {}           # galaxy -> (ip, port, when)
        self.gal_asking = set()      # galaxies being looked up
        self.points = queue.Queue()  # (ship, Point or None) from resolver threads
        self.jobs = queue.Queue()    # functions from other threads, to run on the loop
        self.timers = []             # heap of (when, n, fn), see after()
        self.timer_n = 0
        self.asking = set()
        self.helpers = list(helpers)
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
        for h in self.helpers:
            h.start(self)

    # ------------------------------------------------- to override

    def setup(self):
        """create your tables in self.db (runs once, before the engine opens)"""

    def on_poke(self, her, agent, mark, noun_):
        """a poke to [agent]; return True (or ACK) to ack, False (or NACK,
        or raise) to nack, or DROP for agents you don't run: no answer, so
        they resend"""
        return DROP

    def on_watch(self, her, bone, agent, path_):
        """a subscription; return True (or ACK) to accept (then give facts
        with fact()), False (or NACK) to refuse, or DROP"""
        return DROP

    def on_leave(self, her, bone, agent):
        pass

    def on_plea(self, her, bone, vane, path_, payload):
        """any other request; return ACK, NACK or DROP, or LATER and answer
        it with done(her, bone) later"""
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

    def on_kick(self, her, bone):
        """our subscription on [bone] was kicked (gall's [%x ~])"""

    def on_corked(self, her, bone):
        """our flow [bone] is closed for good"""

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

    def kick(self, her, bone):
        """end their subscription on [bone]"""
        _check(lib.mesa_gall_kick(self.eng, her, bone), "mesa_gall_kick")

    def cork(self, her, bone):
        _check(lib.mesa_flow_cork(self.eng, her, bone), "mesa_flow_cork")

    def plea(self, her, bone, vane, path_, payload):
        """a raw request on our flow [bone] (for vanes other than gall)"""
        _check(lib.mesa_flow_plea(self.eng, her, bone, noun(vane), noun(path(path_)),
                                  noun(payload)), "mesa_flow_plea")

    def boon(self, her, bone, payload):
        """a raw response on their flow [bone]"""
        _check(lib.mesa_flow_boon(self.eng, her, bone, noun(payload)), "mesa_flow_boon")

    def done(self, her, bone, error=None):
        """answer a plea that on_plea returned LATER for: ack it, or nack it
        with an error noun [tag tang]"""
        _check(lib.mesa_flow_done(self.eng, her, bone,
                                  NONE if error is None else noun(error)),
               "mesa_flow_done")

    def peer(self, her):
        """the Point (keys, rift, sponsor) we have for [her], or None"""
        pot = Point()
        return pot if lib.mesa_engine_peer(self.eng, her, C.byref(pot)) else None

    def scry(self, her, path_):
        _check(lib.mesa_keen(self.eng, her, path_.encode()), "mesa_keen")

    def unscry(self, her, path_):
        """stop asking for [path_]; on_scry won't be called for it"""
        _check(lib.mesa_yawn(self.eng, her, path_.encode()), "mesa_yawn")

    # ------------------------------------------------- helper state

    def kv_get(self, k):
        row = self.db.execute("SELECT v FROM mesa_kv WHERE k = ?", (k,)).fetchone()
        return None if row is None else bytes(row[0])

    def kv_put(self, k, v):
        self.db.execute("INSERT OR REPLACE INTO mesa_kv VALUES (?, ?)", (k, v))

    def kv_del(self, k):
        self.db.execute("DELETE FROM mesa_kv WHERE k = ?", (k,))

    def kv_scan(self, pre):
        """(key, value) rows whose key starts with [pre], in order"""
        return [(bytes(k), bytes(v)) for k, v in self.db.execute(
            "SELECT k, v FROM mesa_kv WHERE substr(k, 1, ?) = ? ORDER BY k",
            (len(pre), pre)).fetchall()]

    def after(self, delay, fn):
        """run fn() on the loop, in one transaction, [delay] seconds from now"""
        self.timer_n += 1
        heapq.heappush(self.timers, (time.time() + delay, self.timer_n, fn))

    def call_soon(self, fn):
        """run fn() on the loop, in one transaction; safe from any thread"""
        self.jobs.put(fn)

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
        #  look it up off the loop; meanwhile the datagram goes to the old
        #  address, or nowhere (the engine resends)
        if g not in self.gal_asking:
            self.gal_asking.add(g)
            name = ship_text(Ship(g, 0))[1:] + ".urbit.org"

            def look():
                try:
                    ip = socket.gethostbyname(name)
                except OSError as e:
                    ip = None
                    print("mesa: can't find galaxy %s: %s" % (name, e), file=sys.stderr)
                self.jobs.put(lambda: self._galaxy_found(g, ip))

            threading.Thread(target=look, daemon=True).start()
        return hit[:2] if hit else None

    def _galaxy_found(self, g, ip):
        self.gal_asking.discard(g)
        if ip:
            self.galaxies[g] = (ip, 13337 + g, time.time())

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
            if kind == GALL_POKE:
                mark = Noun(req.mark).text()
                for h in self.helpers:
                    res = h.handle_poke(self, her, bone, agent, mark, Noun(req.noun))
                    if res is not None:
                        return _answer(res)
                return _answer(self.on_poke(her, agent, mark, Noun(req.noun)))
            if kind in (GALL_WATCH, GALL_WATCH_AS):
                p = _path_text(Noun(req.path))
                for h in self.helpers:
                    res = h.handle_watch(self, her, bone, agent, p)
                    if res is not None:
                        return _answer(res)
                return _answer(self.on_watch(her, bone, agent, p))
            if kind == GALL_LEAVE:
                self.on_leave(her, bone, agent)
                return ACK
            return _answer(self.on_plea(her, bone, Noun(vane), Noun(path_),
                                        Noun(payload)))

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
            m = Noun(mark.value).text()

            def fact():
                if not any(h.handle_fact(self, her, bone, m, Noun(val.value))
                           for h in self.helpers):
                    self.on_fact(her, bone, m, Noun(val.value))
            self._guard(fact)
        elif kind == GALL_KICK:
            def kick():
                if not any(h.handle_kick(self, her, bone) for h in self.helpers):
                    self.on_kick(her, bone)
            self._guard(kick)
        else:
            self._guard(self.on_boon, her, bone, Noun(payload))

    def _done(self, ctx, her, bone, error):
        her = Ship(her.lo, her.hi)
        err = None if error == NONE else tang(Noun(error))

        def done():
            if not any(h.handle_done(self, her, bone, err) for h in self.helpers):
                self.on_done(her, bone, err)
        self._guard(done)

    def _corked(self, ctx, her, bone):
        her = Ship(her.lo, her.hi)

        def corked():
            if not any(h.handle_corked(self, her, bone) for h in self.helpers):
                self.on_corked(her, bone)
        self._guard(corked)

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
        her = Ship(her.lo, her.hi)

        def breach():
            for h in self.helpers:
                h.handle_breach(self, her)
            self.on_breach(her)
        self._guard(breach)

    # ------------------------------------------------- the loop

    @staticmethod
    def _now():
        return int(time.time() * 1000)

    def step(self, timeout=0.1):
        """one turn of the loop: datagrams, keys, timers"""
        dea = lib.mesa_engine_deadline(self.eng)
        wait = timeout if dea == NONE else max(0, min(timeout, (dea - self._now()) / 1000))
        if self.timers:
            wait = max(0, min(wait, self.timers[0][0] - time.time()))
        if not self.jobs.empty():
            wait = 0
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
                self._outside(self.on_fd, fd)
        while not self.points.empty():
            who, pot = self.points.get()
            self.asking.discard(who)
            if pot is None:
                lib.mesa_engine_point_missing(self.eng, who)
            else:
                lib.mesa_engine_point(self.eng, C.byref(pot))
        while not self.jobs.empty():
            self._in_txn(self.jobs.get())
        while self.timers and self.timers[0][0] <= time.time():
            self._in_txn(heapq.heappop(self.timers)[2])
        if lib.mesa_engine_deadline(self.eng) <= self._now():
            lib.mesa_engine_wake(self.eng, self._now())
        self._outside(self.on_tick)

    def _in_txn(self, fn):
        """run fn() in a transaction; an exception aborts it and is logged"""
        try:
            with self.txn():
                fn()
        except Exception:
            traceback.print_exc()

    @staticmethod
    def _outside(fn, *a):
        """run an application callback; an exception is logged, and doesn't
        stop the loop"""
        try:
            fn(*a)
        except Exception:
            traceback.print_exc()

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
            self.sock.close()
            self.store.lock.close()          # releases the flock

def _answer(res):
    """a handler's answer: ACK, NACK, LATER or DROP as such; otherwise
    truthy acks and falsy (None included) nacks"""
    if type(res) is int and res in (ACK, NACK, LATER, DROP):
        return res
    return ACK if res else NACK

def _path_text(p):
    segs = []
    while p.is_cell:
        segs.append(p.head.text())
        p = p.tail
    return "/" + "/".join(segs)

# ---------------------------------------------------------------- helpers
#
#  A helper sees requests and responses before the Node's on_* methods:
#  the first to claim one (handle_poke/handle_watch return an answer, the
#  others True) gets it. Pass them as Node(..., helpers=[...]). Their
#  state is in mesa_kv under keys starting with 'a' (the engine's are
#  upper case), in the same format as the Go binding's, so it commits with
#  each ack, survives restarts, and one database works with both.

class Helper:
    def start(self, node):
        """load state; runs once, after the engine opens"""

    def handle_poke(self, node, her, bone, agent, mark, noun_):
        """None: not ours; otherwise an answer, as on_poke's"""
        return None

    def handle_watch(self, node, her, bone, agent, path_):
        return None

    def handle_fact(self, node, her, bone, mark, noun_):
        return False

    def handle_kick(self, node, her, bone):
        return False

    def handle_done(self, node, her, bone, error):
        return False

    def handle_corked(self, node, her, bone):
        return False

    def handle_breach(self, node, her):
        """every helper sees a breach, then on_breach"""

def _ship16(s):
    return s.lo.to_bytes(8, "little") + s.hi.to_bytes(8, "little")

def _ship_of16(b):
    return Ship(int.from_bytes(b[:8], "little"), int.from_bytes(b[8:16], "little"))

def _u64(v):
    return v.to_bytes(8, "big")

def _key(kind, *fields):
    """b'a' kind fields: Ships (16 bytes little-endian), ints (8 bytes
    big-endian), strs (NUL-terminated)"""
    k = b"a" + kind
    for f in fields:
        if isinstance(f, Ship):
            k += _ship16(f)
        elif isinstance(f, int):
            k += _u64(f)
        else:
            k += f.encode() + b"\0"
    return k

def _cut(b):
    i = b.index(b"\0")
    return b[:i].decode(), b[i + 1:]

SUB_WATCHING, SUB_LIVE, SUB_WAITING = 1, 2, 3

class Sub:
    """a subscription of ours: [path] on [ship]'s [agent]"""

    __slots__ = ("ship", "agent", "path", "bone", "state", "tries", "at")

    def __init__(self, ship_, agent, path_):
        self.ship, self.agent, self.path = ship_, agent, path_
        self.bone, self.state, self.tries, self.at = 0, SUB_WAITING, 0, 0

    @property
    def id(self):
        return (self.ship, self.agent, self.path)

    def __repr__(self):
        return "Sub(%s %%%s %s)" % (self.ship, self.agent, self.path)

class Subs(Helper):
    """Keeps subscriptions alive: subscribe() watches on a fresh flow, a
    kick (or the publisher's breach) resubscribes after a backoff, and
    unsubscribe() leaves and corks. Facts on its flows go to on_fact(node,
    sub, mark, noun), not to Node.on_fact; on_watch(node, sub, error) says
    a watch was accepted (error None) or refused (the subscription is
    over); on_kick(node, sub) that one was kicked."""

    def __init__(self, on_fact=None, on_watch=None, on_kick=None, backoff=300):
        self.on_fact, self.on_watch, self.on_kick = on_fact, on_watch, on_kick
        self.backoff = backoff           # most seconds between resubscribes
        self.subs = {}                   # (ship, agent, path) -> Sub
        self.bones = {}                  # (ship, bone) -> Sub
        self.gone = set()                # (ship, bone): left or kicked, until corked

    def _skey(self, s):
        return _key(b"s", s.ship, s.agent, s.path)

    def _save(self, node, s):
        node.kv_put(self._skey(s), _u64(s.bone) + bytes([s.state, s.tries]) + _u64(s.at))

    def _bury(self, node, her, bone):
        self.gone.add((her, bone))
        node.kv_put(_key(b"l", her, bone), b"")

    def start(self, node):
        for k, v in node.kv_scan(b"as"):
            if len(k) < 18 or len(v) != 18:
                continue
            agent, rest = _cut(k[18:])
            p, _ = _cut(rest)
            s = Sub(_ship_of16(k[2:18]), agent, p)
            s.bone, s.state, s.tries = int.from_bytes(v[:8], "big"), v[8], v[9]
            s.at = int.from_bytes(v[10:], "big")
            self.subs[s.id] = s
            if s.state != SUB_WAITING:
                self.bones[(s.ship, s.bone)] = s
        for k, _ in node.kv_scan(b"al"):
            if len(k) == 26:
                self.gone.add((_ship_of16(k[2:18]), int.from_bytes(k[18:], "big")))
        for s in self.subs.values():
            if s.state == SUB_WAITING:
                self._schedule(node, s)

    def subscribe(self, node, her, agent, path_):
        """watch [path_] on [her]'s [agent], unless we already do (in a
        handler, or `with node.txn():`)"""
        if (her, agent, path_) in self.subs:
            return
        s = Sub(her, agent, path_)
        self._watch(node, s)
        self.subs[s.id] = s

    def _watch(self, node, s):
        bone = node.open_flow(s.ship)
        node.watch(s.ship, bone, s.agent, s.path)
        s.bone, s.state = bone, SUB_WATCHING
        self.bones[(s.ship, bone)] = s
        self._save(node, s)

    def unsubscribe(self, node, her, agent, path_):
        """leave [path_] on [her]'s [agent], and close its flow"""
        s = self.subs.pop((her, agent, path_), None)
        if s is None:
            return
        if s.state != SUB_WAITING:
            node.leave(her, s.bone, agent)
            self._cork(node, her, s.bone)
            self.bones.pop((her, s.bone), None)
        node.kv_del(self._skey(s))

    def list(self):
        return sorted(self.subs.values(), key=lambda s: (s.ship.hi, s.ship.lo, s.agent, s.path))

    def _cork(self, node, her, bone):
        try:
            node.cork(her, bone)
        except MesaError:
            pass                         # closing already
        self._bury(node, her, bone)

    def _kicked(self, node, s):
        now = Node._now()
        if s.state == SUB_LIVE and now - s.at >= 60000:
            s.tries = 0
        if s.state != SUB_WAITING:
            self.bones.pop((s.ship, s.bone), None)
        delay = 0 if s.tries == 0 else min(2 ** (s.tries - 1), self.backoff)
        s.at = now + int(delay * 1000)
        s.tries = min(s.tries + 1, 255)
        s.state = SUB_WAITING
        self._save(node, s)
        self._schedule(node, s)

    def _schedule(self, node, s):
        def fire():
            if self.subs.get(s.id) is not s or s.state != SUB_WAITING:
                return                   # unsubscribed, or resubscribed
            if Node._now() < s.at:
                return self._schedule(node, s)
            self._watch(node, s)
        node.after(max(0, (s.at - Node._now()) / 1000), fire)

    def handle_fact(self, node, her, bone, mark, noun_):
        if (her, bone) in self.gone:
            return True
        s = self.bones.get((her, bone))
        if s is None:
            return False
        if self.on_fact:
            self.on_fact(node, s, mark, noun_)
        return True

    def handle_kick(self, node, her, bone):
        if (her, bone) in self.gone:
            return True
        s = self.bones.get((her, bone))
        if s is None:
            return False
        #  gall closes a kicked subscription's flow; a new one resubscribes
        self._cork(node, her, bone)
        if self.on_kick:
            self.on_kick(node, s)
        self._kicked(node, s)
        return True

    def handle_done(self, node, her, bone, error):
        if (her, bone) in self.gone:
            return True
        s = self.bones.get((her, bone))
        if s is None:
            return False
        if s.state != SUB_WATCHING:
            return True
        if error is not None:
            #  refused: over for good
            del self.bones[(her, bone)]
            del self.subs[s.id]
            node.kv_del(self._skey(s))
            self._cork(node, her, bone)
        else:
            s.state, s.at = SUB_LIVE, Node._now()
            self._save(node, s)
        if self.on_watch:
            self.on_watch(node, s, error)
        return True

    def handle_corked(self, node, her, bone):
        if (her, bone) not in self.gone:
            return (her, bone) in self.bones
        self.gone.discard((her, bone))
        node.kv_del(_key(b"l", her, bone))
        return True

    def handle_breach(self, node, her):
        for g in [g for g in self.gone if g[0] == her]:
            self.gone.discard(g)
            node.kv_del(_key(b"l", her, g[1]))
        for s in [s for (w, _), s in self.bones.items() if w == her]:
            self._kicked(node, s)

class Pokes(Helper):
    """Pokes on one flow per (ship, agent), so they arrive in the order
    sent, remembering the flow across restarts. poke() numbers them from
    1 per (ship, agent); on_done(node, her, agent, seq, error) says how
    each went (a breach nacks every unanswered one with ["breach"]). [name]
    keeps two Pokes on one node apart."""

    def __init__(self, on_done=None, name=""):
        self.on_done, self.name = on_done, name
        self.flows = {}                  # (ship, agent) -> [bone, next, acked]
        self.bones = {}                  # (ship, bone) -> agent

    def _pkey(self, her, agent):
        return _key(b"p", self.name, her, agent)

    def _save(self, node, her, agent, f):
        node.kv_put(self._pkey(her, agent), b"".join(_u64(x) for x in f))

    def start(self, node):
        pre = _key(b"p", self.name)
        for k, v in node.kv_scan(pre):
            rest = k[len(pre):]
            if len(rest) < 17 or len(v) != 24:
                continue
            her = _ship_of16(rest)
            agent, _ = _cut(rest[16:])
            f = [int.from_bytes(v[i:i + 8], "big") for i in (0, 8, 16)]
            self.flows[(her, agent)] = f
            self.bones[(her, f[0])] = agent

    def poke(self, node, her, agent, mark, value):
        """poke [her]'s [agent]; its seq (in a handler, or `with node.txn():`)"""
        f = self.flows.get((her, agent))
        if f is None:
            f = [node.open_flow(her), 1, 0]
        node.poke(her, f[0], agent, mark, value)
        seq = f[1]
        f[1] += 1
        self._save(node, her, agent, f)
        self.flows[(her, agent)] = f
        self.bones[(her, f[0])] = agent
        return seq

    def bone(self, her, agent):
        """the flow we poke [her]'s [agent] on, or None"""
        f = self.flows.get((her, agent))
        return f[0] if f else None

    def handle_done(self, node, her, bone, error):
        agent = self.bones.get((her, bone))
        if agent is None:
            return False
        f = self.flows[(her, agent)]
        f[2] += 1
        self._save(node, her, agent, f)
        if self.on_done:
            self.on_done(node, her, agent, f[2], error)
        return True

    def handle_corked(self, node, her, bone):
        return (her, bone) in self.bones

    def handle_breach(self, node, her):
        for (w, agent), f in list(self.flows.items()):
            if w != her:
                continue
            node.kv_del(self._pkey(her, agent))
            del self.flows[(w, agent)]
            self.bones.pop((w, f[0]), None)
            for seq in range(f[2] + 1, f[1]):
                if self.on_done:
                    self.on_done(node, her, agent, seq, ["breach"])

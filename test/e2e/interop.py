"""Interop with a fake ~zod running the mesa-test agent (test/e2e):
flub and spur, and %shut both ways, through the Python binding.

    python3 -I test/e2e/interop.py <galaxy-number> <db>

~zod remembers peers, so use a fresh galaxy (number) per run.
"""

import os
import sys
import time

sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)),
                                "..", "..", "bindings", "python"))
import libmesa as m

ZOD = m.Ship(0, 0)
OURS = "/g/x/0/vault//1/secret/x"         # ours, for ~zod to read
THEIRS = "/g/x/1/mesa-test//1/secret/x"   # ~zod's, for us


class T(m.Node):
    def setup(self):
        self.ev = []
        self.flub = True
        self.kid = 0

    def on_poke(self, her, agent, mark, n):
        if agent != "libmesa":
            return m.FLUB
        if self.flub:
            self.ev.append(("flub",))
            return m.FLUB
        self.ev.append(("poke", n.py()))
        return True

    def on_done(self, her, bone, error):
        self.ev.append(("done", bone, error))

    def on_boon(self, her, bone, payload):
        got = m.Node.parse_key(payload)
        self.ev.append(("key", got))
        if got:
            self.scry_shut(her, got[0], got[1], THEIRS)

    def on_scry(self, her, path_, mark, n):
        self.ev.append(("scry", path_, mark, n.text() if n else None))

    def on_shut_key(self, her, agent, spur):
        self.ev.append(("shut-key", agent, spur))
        return self.kid if agent == "vault" else 0

    def on_peek(self, her, path_):
        if path_.endswith("/vault//1/secret/x"):
            return ("noun", "for arvo")
        return None


def until(n, cond, secs=60):
    end = time.time() + secs
    while time.time() < end:
        n.step(0.01)
        got = cond()
        if got:
            return got
    raise SystemExit("timed out")


def pick(ev, kind):
    for i, e in enumerate(ev):
        if e[0] == kind:
            return ev.pop(i)
    return None


def poke(n, value):
    with n.txn():
        bone = n.open_flow(ZOD)
        n.poke(ZOD, bone, "mesa-test", "noun", value)
    e = until(n, lambda: pick(n.ev, "done"))
    assert e[2] is None, "poke %r nacked: %r" % (value, e[2])


def main():
    g, db = int(sys.argv[1]), sys.argv[2]
    n = T(m.Identity.fake(m.Ship(g, 0)), db)
    print("we are", n.our)

    #  flub: ~zod pokes our %libmesa back; we flub, it stops; we spur, it
    #  resends at once
    #
    poke(n, "poke-back")
    until(n, lambda: pick(n.ev, "flub"))
    print("flubbed ~zod's poke")
    end = time.time() + 20
    while time.time() < end:
        n.step(0.01)
    assert not pick(n.ev, "poke"), "delivered while flubbed"
    n.flub = False
    t0 = time.time()
    with n.txn():
        n.spur("libmesa")
    #  (a message already sent waits for Arvo's ~m2 retry timer)
    got = until(n, lambda: pick(n.ev, "poke"), secs=180)
    print("spurred: ~zod resent in %.1fs: %r" % (time.time() - t0, got[1]))

    #  %shut, theirs: ~zod's gall gives us the key, we read it
    #
    poke(n, "germ")
    with n.txn():
        n.request_key(ZOD, n.open_flow(ZOD), THEIRS)
    key = until(n, lambda: pick(n.ev, "key"))
    assert key[1], "~zod refused the key"
    print("~zod's key: kid %d" % key[1][0])
    got = until(n, lambda: pick(n.ev, "scry"))
    print("read ~zod's %%shut %s: %r" % (got[1], got[2:]))
    assert got[3] == "treasure", got

    #  %shut, ours: ~zod's gall asks us for the key, then reads
    #
    with n.txn():
        n.kid = n.shut_add("/g/x/0/vault//1/secret", os.urandom(32))
    poke(n, ("shut-read", n.our.lo | (n.our.hi << 64), m.path(OURS)))
    ask = until(n, lambda: pick(n.ev, "shut-key"))
    print("~zod asked for our key:", ask[1:])
    print("now check zod.log for [%mesa-test-arvo /shut ...] with 'for arvo'")
    end = time.time() + 15
    while time.time() < end:
        n.step(0.01)
    n.close()
    print("interop ok")


if __name__ == "__main__":
    main()

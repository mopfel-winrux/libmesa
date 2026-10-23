"""Tests for the Python binding: fake galaxies talking over localhost
(fake galaxy ~g is at 127.0.0.1:31337+g), driven from this thread.

    python3 -m unittest bindings/python/test_bindings.py
"""

import os
import socket
import sys
import tempfile
import time
import unittest

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import libmesa as m
import tlon

def fake(g):
    return m.Identity.fake(m.Ship(g, 0))

class Feed(m.Node):
    """a publisher: accepts watches and pokes on %feed"""

    def setup(self):
        self.ev = []

    def on_watch(self, her, bone, agent, path_):
        if agent != "feed":
            return m.DROP
        self.ev.append(("watch", bone, path_))
        self.fact(her, bone, "noun", "welcome")
        return True

    def on_leave(self, her, bone, agent):
        self.ev.append(("leave", bone))

    def on_poke(self, her, agent, mark, n):
        if agent != "feed":
            return m.DROP
        self.ev.append(("poke", n.text()))
        return n.text() != "bad"

class Plain(m.Node):
    def setup(self):
        self.ev = []

    def on_done(self, her, bone, error):
        self.ev.append(("done", bone, error))

    def on_fact(self, her, bone, mark, n):
        self.ev.append(("fact", mark, n.py()))

class Base(unittest.TestCase):
    def setUp(self):
        self.dir = tempfile.TemporaryDirectory()
        self.nodes = []

    def tearDown(self):
        for n in self.nodes:
            n.close()
        self.dir.cleanup()

    def node(self, cls, g, name, **k):
        n = cls(fake(g), os.path.join(self.dir.name, name), **k)
        self.nodes.append(n)
        return n

    def drop(self, n):
        n.close()
        self.nodes.remove(n)

    def until(self, cond, secs=20):
        end = time.time() + secs
        while time.time() < end:
            for n in self.nodes:
                n.step(0.005)
            got = cond()
            if got:
                return got
        self.fail("timed out")

    def pick(self, ev, kind):
        """the first [kind] event in ev, removed"""
        for i, e in enumerate(ev):
            if e[0] == kind:
                return ev.pop(i)
        return None

class TestSubs(Base):
    def test_resubscribe_and_restart(self):
        pub = self.node(Feed, 221, "pub.db")
        ev = []

        def make():
            return m.Subs(on_fact=lambda n, s, mark, x: ev.append(("fact", s.path, x.text())),
                          on_watch=lambda n, s, e: ev.append(("watched", s.path, e)),
                          on_kick=lambda n, s: ev.append(("kicked", s.path)),
                          backoff=2)

        subs = make()
        a = self.node(m.Node, 220, "a.db", helpers=[subs])
        with a.txn():
            subs.subscribe(a, pub.our, "feed", "/news")
            subs.subscribe(a, pub.our, "feed", "/news")      # no-op
        w1 = self.until(lambda: self.pick(pub.ev, "watch"))
        self.assertEqual(w1[2], "/news")
        self.until(lambda: self.pick(ev, "watched"))
        self.assertEqual(self.until(lambda: self.pick(ev, "fact"))[2], "welcome")

        #  kicked: right back, on a new flow
        with pub.txn():
            pub.kick(a.our, w1[1])
        w2 = self.until(lambda: self.pick(pub.ev, "watch"))
        self.assertNotEqual(w2[1], w1[1])
        self.assertTrue(self.pick(ev, "kicked"))

        #  kicked again at once: after a backoff
        t0 = time.time()
        with pub.txn():
            pub.kick(a.our, w2[1])
        w3 = self.until(lambda: self.pick(pub.ev, "watch"))
        self.assertGreater(time.time() - t0, 0.9)

        #  restart: facts on the flow still reach on_fact
        self.drop(a)
        ev.clear()
        subs = make()
        a = self.node(m.Node, 220, "a.db", helpers=[subs])
        self.assertEqual([s.path for s in subs.list()], ["/news"])
        with pub.txn():
            pub.fact(a.our, w3[1], "noun", "later")
        self.assertEqual(self.until(lambda: self.pick(ev, "fact"))[2], "later")

        #  unsubscribe: leave, and the flow closes
        with a.txn():
            subs.unsubscribe(a, pub.our, "feed", "/news")
        self.assertEqual(self.until(lambda: self.pick(pub.ev, "leave"))[1], w3[1])
        self.assertEqual(subs.list(), [])

class TestPokes(Base):
    def test_ordered_numbered(self):
        pub = self.node(Feed, 223, "pub.db")
        done = []
        pk = m.Pokes(on_done=lambda n, her, agent, seq, e: done.append((seq, e)))
        a = self.node(m.Node, 222, "a.db", helpers=[pk])
        with a.txn():
            seqs = [pk.poke(a, pub.our, "feed", "noun", t) for t in ("one", "bad", "three")]
        self.assertEqual(seqs, [1, 2, 3])
        self.until(lambda: len(done) == 3)
        self.assertEqual([p[1] for p in pub.ev], ["one", "bad", "three"])
        self.assertEqual([s for s, _ in done], [1, 2, 3])
        self.assertIsNone(done[0][1])
        self.assertIsNotNone(done[1][1])
        bone = pk.bone(pub.our, "feed")

        self.drop(a)
        pk = m.Pokes(on_done=lambda n, her, agent, seq, e: done.append((seq, e)))
        a = self.node(m.Node, 222, "a.db", helpers=[pk])
        self.assertEqual(pk.bone(pub.our, "feed"), bone)
        with a.txn():
            self.assertEqual(pk.poke(a, pub.our, "feed", "noun", "four"), 4)
        self.until(lambda: len(done) == 4)

class TestDMs(Base):
    def test_round_trip(self):
        got_a, got_b, sent = [], [], []

        def b_message(node, msg):
            got_b.append((msg.ship, msg.text, msg.id))
            bd.send(node, msg.ship, "you said: " + msg.text)

        ad = tlon.DMs(on_message=lambda n, msg: got_a.append((msg.ship, msg.text)),
                      on_sent=lambda n, her, id_, e: sent.append((id_, e)))
        bd = tlon.DMs(on_message=b_message)
        a = self.node(Plain, 224, "a.db", helpers=[ad])
        b = self.node(m.Node, 225, "b.db", helpers=[bd])

        with a.txn():
            id_ = ad.send(a, b.our, "hello\nthere")
        self.until(lambda: got_b and got_a and sent)
        self.assertEqual(got_b, [(a.our, "hello\nthere", id_)])
        self.assertEqual(sent, [(id_, None)])
        self.assertEqual(got_a, [(b.our, "you said: hello\nthere")])

        #  Tlon resending it on a new flow: acked, not delivered again
        us = tlon.atom_ship(a.our)
        sent_da = int(id_)
        with a.txn():
            bone = a.open_flow(b.our)
            a.poke(b.our, bone, tlon.AGENT, tlon.MARK,
                   ((us, sent_da), ("add", ((tlon.story("hello\nthere"), us, sent_da),
                                            ("chat", None), None, None), None)))
        d = self.until(lambda: self.pick(a.ev, "done"))
        self.assertIsNone(d[2])
        for _ in range(30):
            b.step(0.01)
        self.assertEqual(len(got_b), 1)

        #  Tlon's version check
        with a.txn():
            bone = a.open_flow(b.our)
            a.watch(b.our, bone, tlon.AGENT, tlon.VERSION_PATH)
        f = self.until(lambda: self.pick(a.ev, "fact"))
        self.assertEqual(f[1:], ("noun", 2))

class TestGalaxyDNS(Base):
    def test_lookup_off_the_loop(self):
        a = self.node(m.Node, 226, "a.db")
        a.fake = False                   # as if live: look ~zod up
        slow = socket.gethostbyname

        def lookup(name):
            time.sleep(0.5)
            return "192.0.2.1"
        socket.gethostbyname = lookup
        try:
            t0 = time.time()
            self.assertIsNone(a._galaxy(0))
            self.assertLess(time.time() - t0, 0.1)
            self.until(lambda: 0 in a.galaxies, secs=5)
            self.assertEqual(a._galaxy(0), ("192.0.2.1", 13337))
        finally:
            socket.gethostbyname = slow
            a.fake = True

if __name__ == "__main__":
    unittest.main()

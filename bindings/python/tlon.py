"""tlon: Tlon direct messages over libmesa.

Speaks Tlon's DM protocol (tloncorp/tlon-apps, desk/app/chat.hoon): DMs
is a libmesa helper that runs as agent %chat, takes %chat-dm-diff pokes,
answers Tlon's version check (a watch on /~/negotiate/version/chat-dms)
with %chat-dms version 2, and sends DMs on one ordered flow per ship.

    import libmesa, tlon

    def message(node, m):
        dms.send(node, m.ship, "you said: " + m.text)

    dms = tlon.DMs(on_message=message)
    node = libmesa.Node(libmesa.Identity.comet("bot.key"), "bot.db", helpers=[dms])
    node.run()

A message is handed to on_message once: the record that dedupes it
commits in the same transaction as its ack, and so does whatever
on_message writes or sends. Its state shares the Go package's format.
"""

import libmesa as m

AGENT = "chat"
MARK = "chat-dm-diff-2"              # what we send; we take any chat-dm-diff
VERSION = 2
VERSION_PATH = "/~/negotiate/version/chat-dms"

def lst(items):
    """a Hoon list: (a b c ~)"""
    return tuple(items) + (None,) if items else None

def atom_ship(who):
    return who.lo | (who.hi << 64)

def ship_of(n):
    v = int(n)
    return m.Ship(v & ((1 << 64) - 1), v >> 64)

def story(text):
    """a story from plain text: one paragraph, line breaks as %break"""
    inlines = []
    for i, line in enumerate(text.split("\n")):
        if i:
            inlines.append(("break", None))
        if line:
            inlines.append(line)
    return lst([("inline", lst(inlines or [""]))])

def story_text(n):
    """the plain text of a story, best effort"""
    out = []

    def inline(i, d):
        if d > 32:
            return ""
        if not i.is_cell:
            return i.text()
        tag, p = i.head.text(), i.tail
        if tag in ("italics", "bold", "strike", "blockquote"):
            return inlines(p, d + 1)
        if tag == "task":
            return inlines(p.tail, d + 1)
        if tag in ("inline-code", "code", "tag"):
            return ("#" if tag == "tag" else "") + p.text()
        if tag == "ship":
            return str(ship_of(p))
        if tag == "break":
            return "\n"
        if tag == "link":
            return p.tail.text() or p.head.text()
        if tag == "block":
            return p.tail.text()
        return ""

    def inlines(l, d):
        s = []
        while l.is_cell:
            s.append(inline(l.head, d))
            l = l.tail
        return "".join(s)

    while n.is_cell:
        verse = n.head
        if verse.is_cell:
            kind = verse.head.text()
            if kind == "inline":
                out.append(inlines(verse.tail, 0))
            elif kind == "block":
                out.append("[%s]" % verse.tail.head.text())
        n = n.tail
    return "\n".join(t for t in out if t)

class Message:
    """a DM we got: [ship] sent [text] at [sent] (Unix seconds); [id] is
    its @da in decimal, [story] the story itself (borrowed for the call)"""

    __slots__ = ("ship", "sent", "id", "text", "story")

    def __init__(self, ship_, sent, id_, text, story_):
        self.ship, self.sent, self.id, self.text, self.story = ship_, sent, id_, text, story_

    def __repr__(self):
        return "Message(%s %r)" % (self.ship, self.text)

class DMs(m.Helper):
    """Tlon DMs. on_message(node, message) runs in the transaction that
    acks it (raise to nack it); messages Tlon resends (same author and
    sent time) are acked without calling it again. on_sent(node, ship, id,
    error) says our DM [id] was delivered (error None) or refused."""

    def __init__(self, on_message=None, on_sent=None):
        self.on_message, self.on_sent = on_message, on_sent
        self.pokes = m.Pokes(on_done=self._done, name="tlon-dms")
        self.last_da = 0

    #  keys: b"at" ship id NUL: a message we got; b"aT" ship seq: the id
    #  of our message [seq], until it's answered

    def start(self, node):
        self.pokes.start(node)

    def handle_watch(self, node, her, bone, agent, path_):
        if agent != AGENT or path_ != VERSION_PATH:
            return None
        node.fact(her, bone, "noun", VERSION)
        return m.ACK

    def handle_poke(self, node, her, bone, agent, mark, n):
        if agent != AGENT:
            return None
        if not mark.startswith("chat-dm-diff"):
            return m.ACK                 # rsvps, (un)blocks: nothing to do
        #  [id=[author sent] %add essay=[[content author sent] kind meta blob] time]
        delta = n.tail
        if not delta.is_cell or delta.head.text() != "add":
            return m.ACK                 # deletes, reactions, replies
        memo = delta.tail.head.head
        sent = int(memo.tail.tail)
        k = b"at" + m._ship16(her) + str(sent).encode() + b"\0"
        if node.kv_get(k) is not None:
            return m.ACK
        node.kv_put(k, b"")
        if self.on_message:
            self.on_message(node, Message(her, m.unix(sent), str(sent),
                                          story_text(memo.head).strip(), memo.head))
        return m.ACK

    def next_da(self):
        """a fresh @da per message we send: it's the message's id"""
        d = max(m.da(), self.last_da + (1 << 48))
        self.last_da = d
        return d

    def send(self, node, her, text):
        """DM [text] to [her]; its id (in a handler, or `with node.txn():`)"""
        return self.send_story(node, her, story(text))

    def send_story(self, node, her, story_):
        sent = self.next_da()
        us = atom_ship(node.our)
        #  [id=[author sent] %add essay=[[content author sent] kind meta blob] time=~]
        seq = self.pokes.poke(node, her, AGENT, MARK,
                              ((us, sent),
                               ("add", ((story_, us, sent), ("chat", None), None, None), None)))
        node.kv_put(b"aT" + m._ship16(her) + m._u64(seq), str(sent).encode())
        return str(sent)

    def _done(self, node, her, agent, seq, error):
        k = b"aT" + m._ship16(her) + m._u64(seq)
        id_ = (node.kv_get(k) or b"").decode()
        node.kv_del(k)
        if self.on_sent:
            self.on_sent(node, her, id_, error)

    def handle_done(self, node, her, bone, error):
        return self.pokes.handle_done(node, her, bone, error)

    def handle_corked(self, node, her, bone):
        return self.pokes.handle_corked(node, her, bone)

    def handle_breach(self, node, her):
        self.pokes.handle_breach(node, her)

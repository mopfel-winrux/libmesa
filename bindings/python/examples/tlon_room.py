"""tlon_room: a chat room that lives in Tlon DMs.

DM the room's ship from the Tlon app and you're in: everything you write
is relayed, as a DM from the room, to everyone else who has written to it.
Lines typed into the room's own terminal go to everyone.

    python3 tlon_room.py -k ~sampel-palnet.key -s room.db [--fresh]
    python3 tlon_room.py -c -s room.db              # as a fresh comet
    ... --web 127.0.0.1:8474                        # and a web page for it

In a DM to the room:  /who  /leave  /help

It speaks Tlon's DM protocol (tloncorp/tlon-apps, desk/app/chat.hoon):
it runs as agent %chat, takes %chat-dm-diff-2 pokes, sends the same back,
and answers Tlon's version check (a watch on /~/negotiate/version/chat-dms)
with %chat-dms version 2. Each message is recorded and relayed in one
transaction with its ack, so a crash never loses or repeats one.
"""

import argparse
import http.server
import json
import os
import queue
import sqlite3
import sys
import threading
import time

sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), ".."))
import libmesa as m

AGENT = "chat"
DM_MARKS = ("chat-dm-diff-2", "chat-dm-diff-1", "chat-dm-diff")
PENDING_MAX = 50          # stop relaying to a member this far behind

HELP = ("This is a chat room: messages you send here go to everyone else in "
        "it. /who lists members, /leave leaves (write again to come back).")

# ---------------------------------------------------------------- stories

def atom_ship(who):
    return who.lo | (who.hi << 64)

def ship_of(n):
    """a Ship from an @p noun"""
    v = int(n)
    return m.Ship(v & ((1 << 64) - 1), v >> 64)

def lst(items):
    """a Hoon list: (a b c ~)"""
    return tuple(items) + (None,) if items else None

def story(inlines):
    """a story of one paragraph: ~[[%inline inlines]]"""
    return lst([("inline", lst(inlines))])

def story_text(n, depth=0):
    """the plain text of a story, best effort"""
    out = []

    def inline(i, d):
        if d > 32:
            return ""
        if not i.is_cell:
            return i.text()
        tag = i.head.text()
        p = i.tail
        if tag in ("italics", "bold", "strike", "blockquote"):
            return inlines(p, d + 1)
        if tag == "task":
            return inlines(p.tail, d + 1)
        if tag in ("inline-code", "code"):
            return p.text()
        if tag == "ship":
            return str(ship_of(p))
        if tag == "break":
            return "\n"
        if tag == "tag":
            return "#" + p.text()
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
        kind = verse.head.text()
        if kind == "inline":
            out.append(inlines(verse.tail, 0))
        elif kind == "block":
            out.append("[%s]" % verse.tail.head.text())
        n = n.tail
    return "\n".join(t for t in out if t)

# -------------------------------------------------------------- the room

class Room(m.Node):
    def setup(self):
        self.db.executescript("""
            CREATE TABLE IF NOT EXISTS member (
              ship TEXT PRIMARY KEY, bone INTEGER, pending INTEGER DEFAULT 0,
              joined REAL);
            CREATE TABLE IF NOT EXISTS message (
              id INTEGER PRIMARY KEY, ship TEXT, text TEXT, at REAL);
            CREATE TABLE IF NOT EXISTS seen (
              author TEXT, sent TEXT, PRIMARY KEY (author, sent));
        """)
        self.last_da = 0
        self.input = None
        self.posts = queue.Queue()      #  from the web page

    # ------------------------------------------------ talking to members

    def next_da(self):
        """a fresh @da for each message we send: it's the message id"""
        d = max(m.da(), self.last_da + (1 << 48))
        self.last_da = d
        return d

    def dm(self, who, inlines):
        """send a DM (a list of inlines) to [who], on their flow"""
        name = str(who)
        row = self.db.execute("SELECT bone, pending FROM member WHERE ship = ?",
                              (name,)).fetchone()
        if row is None:
            bone = self.open_flow(who)
            self.db.execute("INSERT INTO member (ship, bone, joined) VALUES (?, ?, ?)",
                            (name, bone, time.time()))
            pending = 0
        else:
            bone, pending = row
            if bone is None:
                bone = self.open_flow(who)
                self.db.execute("UPDATE member SET bone = ? WHERE ship = ?", (bone, name))
        if pending >= PENDING_MAX:
            return
        sent = self.next_da()
        us = atom_ship(self.our)
        #  [id=[author sent] %add essay=[[content author sent] kind meta blob] time=~]
        diff = ((us, sent),
                ("add", ((story(inlines), us, sent), ("chat", None), None, None), None))
        self.poke(who, bone, AGENT, "chat-dm-diff-2", diff)
        self.db.execute("UPDATE member SET pending = pending + 1 WHERE ship = ?", (name,))

    def members(self):
        return [r[0] for r in self.db.execute("SELECT ship FROM member ORDER BY joined")]

    def say(self, sender, text):
        """[sender] (a Ship, or None for the room itself) says [text]"""
        name = str(sender) if sender else None
        self.db.execute("INSERT INTO message (ship, text, at) VALUES (?, ?, ?)",
                        (name or "room", text, time.time()))
        print("%s: %s" % (name or "(room)", text), flush=True)
        for who in self.members():
            if who == name:
                continue
            body = [text] if sender is None else [("ship", atom_ship(sender)), ": " + text]
            self.dm(m.ship(who), body)

    def join(self, who):
        name = str(who)
        if self.db.execute("SELECT 1 FROM member WHERE ship = ?", (name,)).fetchone():
            return
        others = self.members()
        self.db.execute("INSERT INTO member (ship, joined) VALUES (?, ?)", (name, time.time()))
        print("-- %s joined" % name, flush=True)
        self.dm(who, ["Welcome! %s" % HELP,
                      " %d other%s here." % (len(others), "" if len(others) == 1 else "s")])
        for o in others:
            self.dm(m.ship(o), [("ship", atom_ship(who)), " joined the room."])

    def part(self, who):
        name = str(who)
        if not self.db.execute("SELECT 1 FROM member WHERE ship = ?", (name,)).fetchone():
            return
        self.db.execute("DELETE FROM member WHERE ship = ?", (name,))
        print("-- %s left" % name, flush=True)
        for o in self.members():
            self.dm(m.ship(o), [("ship", atom_ship(who)), " left the room."])

    # ------------------------------------------------ the %chat agent

    def on_watch(self, her, bone, agent, path):
        #  Tlon's version check: we speak %chat-dms version 2
        if agent == AGENT and path == "/~/negotiate/version/chat-dms":
            self.fact(her, bone, "noun", 2)
            return True
        return m.DROP

    def on_poke(self, her, agent, mark, n):
        if agent != AGENT:
            return m.DROP

        if mark in DM_MARKS:
            delta = n.tail
            if delta.head.text() != "add":
                return True                     #  deletes, reactions: fine
            memo = delta.tail.head.head
            sent = str(int(memo.tail.tail))
            #  one message, once (ames already dedupes; this covers Tlon's
            #  own resends on a new flow)
            if self.db.execute("SELECT 1 FROM seen WHERE author = ? AND sent = ?",
                               (str(her), sent)).fetchone():
                return True
            self.db.execute("INSERT INTO seen VALUES (?, ?)", (str(her), sent))
            text = story_text(memo.head).strip()
            self.join(her)
            if text == "/who":
                self.dm(her, ["In the room: " + ", ".join(self.members())])
            elif text == "/leave":
                self.dm(her, ["You've left the room. Write again to come back."])
                self.part(her)
            elif text == "/help":
                self.dm(her, [HELP])
            elif text:
                self.say(her, text)
            return True

        if mark == "chat-dm-rsvp":
            ok = int(n.tail) == 0                #  a loobean: %.y is 0
            if ok:
                self.join(her)
            else:
                self.part(her)
            return True

        if mark in ("chat-blocked", "chat-unblocked"):
            if mark == "chat-blocked":
                self.part(her)
            return True

        return True                             #  other %chat pokes: noted

    def on_done(self, her, bone, error):
        self.db.execute("UPDATE member SET pending = max(0, pending - 1) WHERE ship = ?",
                        (str(her),))
        if error:
            print("!! %s refused a message: %s" % (her, " / ".join(error[:2])), flush=True)

    # ------------------------------------------------ the terminal and web

    def on_tick(self):
        #  the web page's posts, each in its own transaction
        while not self.posts.empty():
            text = self.posts.get()
            with self.txn():
                self.say(None, text)

    def extra_fds(self):
        return [self.input] if self.input else []

    def on_fd(self, fd):
        line = fd.readline()
        if not line:
            self.input = None
            return
        line = line.strip()
        if line == "/who":
            print("members:", ", ".join(self.members()) or "none", flush=True)
        elif line:
            with self.txn():
                self.say(None, line)

# ---------------------------------------------------------------- the web page

PAGE = r"""<!doctype html>
<html lang="en"><head><meta charset="utf-8">
<meta name="viewport" content="width=device-width,initial-scale=1">
<title>Chat Room</title>
<style>
:root{--bg:#f5f4f0;--panel:#fff;--ink:#1c1b19;--mute:#77746c;--line:#e3e0d8;
--acc:#3b5bdb;--mine:#eef2ff}
@media (prefers-color-scheme:dark){:root{--bg:#121211;--panel:#1c1c1a;--ink:#ecebe5;
--mute:#99968e;--line:#2c2b28;--acc:#8ea2ff;--mine:#1f2440}}
*{box-sizing:border-box}
html,body{height:100%;margin:0}
body{background:var(--bg);color:var(--ink);font:15px/1.45 ui-sans-serif,system-ui,
-apple-system,sans-serif;display:flex;justify-content:center}
.app{width:100%;max-width:980px;height:100%;display:grid;grid-template-columns:1fr 240px;
grid-template-rows:auto 1fr auto;gap:0}
header{grid-column:1/3;padding:16px 18px 12px;border-bottom:1px solid var(--line);
display:flex;align-items:baseline;gap:12px;flex-wrap:wrap}
header h1{font-size:20px;margin:0}
.room{font:12.5px ui-monospace,Menlo,monospace;color:var(--mute);word-break:break-all}
.dot{width:8px;height:8px;border-radius:50%;background:#2f9e44;display:inline-block;
margin-left:auto;align-self:center}
.dot.off{background:#c92a2a}
#log{overflow-y:auto;padding:14px 18px;display:flex;flex-direction:column;gap:2px}
.msg{padding:6px 10px;border-radius:8px;max-width:100%}
.msg.room{background:var(--mine)}
.msg .who{font:12.5px ui-monospace,Menlo,monospace;color:var(--acc);margin-right:8px}
.msg.room .who{color:var(--mute)}
.msg .t{color:var(--mute);font-size:11.5px;float:right;margin-left:10px}
.msg .x{white-space:pre-wrap;word-break:break-word}
.msg.new{animation:hi 1.4s ease-out}
@keyframes hi{from{background:var(--mine)}}
.empty{color:var(--mute);text-align:center;margin:auto}
aside{grid-row:2/4;grid-column:2;border-left:1px solid var(--line);padding:14px 16px;
overflow-y:auto}
aside h2{font-size:12px;text-transform:uppercase;letter-spacing:.06em;color:var(--mute);
margin:0 0 8px}
aside li{list-style:none;font:12.5px ui-monospace,Menlo,monospace;padding:3px 0;
word-break:break-all}
aside ul{margin:0;padding:0}
aside p{font-size:13px;color:var(--mute)}
form{display:flex;gap:8px;padding:12px 18px 16px;border-top:1px solid var(--line)}
input{flex:1;font:inherit;padding:10px 12px;border-radius:8px;border:1px solid var(--line);
background:var(--panel);color:var(--ink)}
button{font:inherit;padding:10px 16px;border-radius:8px;border:0;background:var(--acc);
color:#fff;cursor:pointer}
button:disabled{opacity:.5}
@media (max-width:700px){.app{grid-template-columns:1fr}
aside{display:none}form{grid-column:1}}
</style></head><body><div class="app">
<header><h1>Chat Room</h1><span class="room" id="room"></span><span class="dot" id="dot"></span></header>
<div id="log"></div>
<aside><h2>In the room</h2><ul id="members"></ul>
<p>DM the room from Tlon to join.</p></aside>
<form id="f"><input id="in" placeholder="Write to the room" autocomplete="off" maxlength="2000">
<button id="go">Send</button></form>
</div><script>
let last=0,first=true;const log=document.getElementById('log');
function t(at){const d=new Date(at*1000);return d.toLocaleTimeString([],{hour:'2-digit',minute:'2-digit'})}
function el(tag,cls,txt){const e=document.createElement(tag);if(cls)e.className=cls;
if(txt!=null)e.textContent=txt;return e}
async function poll(){try{
 const r=await fetch('state?after='+last,{cache:'no-store'});const d=await r.json();
 document.getElementById('room').textContent=d.room;
 document.getElementById('dot').className='dot';
 const ul=document.getElementById('members');ul.replaceChildren();
 for(const m of d.members)ul.append(el('li','',m));
 if(!d.members.length)ul.append(el('li','','nobody yet'));
 const stick=log.scrollTop+log.clientHeight>=log.scrollHeight-40;
 if(first&&!d.messages.length)log.append(el('div','empty','No messages yet.'));
 for(const m of d.messages){
  const e=log.querySelector('.empty');if(e)e.remove();
  const room=m.ship==='room',div=el('div','msg'+(room?' room':'')+(first?'':' new'));
  div.append(el('span','t',t(m.at)),el('span','who',room?'room':m.ship),el('span','x',m.text));
  log.append(div);last=m.id}
 if(stick||first)log.scrollTop=log.scrollHeight;first=false;
}catch(x){document.getElementById('dot').className='dot off'}}
document.getElementById('f').onsubmit=async e=>{e.preventDefault();
 const i=document.getElementById('in'),b=document.getElementById('go');const v=i.value.trim();
 if(!v)return;b.disabled=true;
 try{await fetch('say',{method:'POST',headers:{'Content-Type':'application/json'},
 body:JSON.stringify({text:v})});i.value='';setTimeout(poll,300)}finally{b.disabled=false;i.focus()}};
poll();setInterval(poll,1500);
</script></body></html>
"""

def serve_web(room, db_path, addr):
    """the page, in a thread: it reads its own connection to the database,
    and hands posts to the room's loop"""
    host, _, port = addr.rpartition(":")
    db = sqlite3.connect("file:%s?mode=ro" % db_path, uri=True, check_same_thread=False)
    lock = threading.Lock()
    name = str(room.our)

    class H(http.server.BaseHTTPRequestHandler):
        def reply(self, code, typ, body):
            self.send_response(code)
            self.send_header("Content-Type", typ)
            self.send_header("Content-Length", str(len(body)))
            self.send_header("Cache-Control", "no-store")
            self.end_headers()
            self.wfile.write(body)

        def do_GET(self):
            if self.path in ("/", "/index.html"):
                return self.reply(200, "text/html; charset=utf-8", PAGE.encode())
            if self.path.startswith("/state"):
                try:
                    after = int(self.path.split("after=", 1)[1]) if "after=" in self.path else 0
                except ValueError:
                    after = 0
                with lock:
                    msgs = db.execute("SELECT id, ship, text, at FROM message WHERE id > ? "
                                      "ORDER BY id LIMIT 500", (after,)).fetchall()
                    mems = [r[0] for r in db.execute("SELECT ship FROM member ORDER BY joined")]
                body = json.dumps({
                    "room": name, "members": mems,
                    "messages": [{"id": i, "ship": s, "text": t, "at": a}
                                 for i, s, t, a in msgs]}).encode()
                return self.reply(200, "application/json", body)
            self.reply(404, "text/plain", b"not found\n")

        def do_POST(self):
            if self.path != "/say":
                return self.reply(404, "text/plain", b"not found\n")
            try:
                n = min(int(self.headers.get("Content-Length", 0)), 16384)
                text = json.loads(self.rfile.read(n))["text"].strip()[:2000]
            except (ValueError, KeyError, TypeError):
                return self.reply(400, "text/plain", b"bad request\n")
            if text:
                room.posts.put(text)
            self.reply(204, "text/plain", b"")

        def log_message(self, *a):
            pass

    srv = http.server.ThreadingHTTPServer((host or "127.0.0.1", int(port)), H)
    threading.Thread(target=srv.serve_forever, daemon=True).start()
    print("web page: http://%s:%s/" % (host or "127.0.0.1", port), flush=True)

def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    who = ap.add_mutually_exclusive_group(required=True)
    who.add_argument("-k", metavar="KEYFILE", help="a planet, star or moon keyfile")
    who.add_argument("-c", action="store_true", help="run as a comet (keys in DB.key)")
    who.add_argument("-F", metavar="SHIP", help="a fake ship, for `urbit -F`")
    ap.add_argument("-s", metavar="DB", required=True, help="state (SQLite)")
    ap.add_argument("--fresh", action="store_true",
                    help="first run of an identity that has never run before")
    ap.add_argument("--input", metavar="FILE", default="-",
                    help="where to read room messages from (default stdin)")
    ap.add_argument("--web", metavar="[ADDR:]PORT",
                    help="serve a web page for the room (default address 127.0.0.1)")
    a = ap.parse_args()

    if a.k:
        ident = m.Identity.keyfile(a.k)
    elif a.c:
        ident = m.Identity.comet(a.s + ".key")
    else:
        ident = m.Identity.fake(a.F)

    room = Room(ident, a.s, breached=a.fresh)
    room.input = sys.stdin if a.input == "-" else open(a.input)
    if a.web:
        serve_web(room, a.s, a.web if ":" in a.web else "127.0.0.1:" + a.web)
    print("room %s is open; DM it from Tlon" % room.our, flush=True)
    room.run()

if __name__ == "__main__":
    main()

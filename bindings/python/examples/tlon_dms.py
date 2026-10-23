"""tlon_dms: direct messages with Tlon users, from a web page.

Your ship (a moon, a comet, a planet) runs here instead of in Urbit; Tlon
users DM it as they would anyone, and you read and answer at a web page:
one conversation per ship, each just between the two of you.

    python3 tlon_dms.py -k ~sampel-palnet.key -s dms.db --web 127.0.0.1:8474
    python3 tlon_dms.py -c -s dms.db --web 8474     # as a fresh comet

Tlon's DM protocol is the tlon module's (../tlon.py). A message is stored
in the same transaction as its ack, so a crash never loses or repeats one.
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
import tlon

class DMs(m.Node):
    def __init__(self, *a, **k):
        self.dms = tlon.DMs(on_message=self.got, on_sent=self.sent)
        super().__init__(*a, helpers=[self.dms], **k)

    def setup(self):
        self.db.executescript("""
            CREATE TABLE IF NOT EXISTS dm (
              ship TEXT PRIMARY KEY, bone INTEGER, last REAL, unread INTEGER DEFAULT 0);
            CREATE TABLE IF NOT EXISTS dm_msg (
              id INTEGER PRIMARY KEY, ship TEXT, mine INTEGER, text TEXT, at REAL,
              sent TEXT, status TEXT);
            CREATE UNIQUE INDEX IF NOT EXISTS dm_msg_sent ON dm_msg (ship, mine, sent);
        """)
        self.todo = queue.Queue()      #  from the web page: ("send", ship, text), ("read", ship)

    def chat(self, name):
        """the conversation with [name], made if new"""
        self.db.execute("INSERT OR IGNORE INTO dm (ship, last) VALUES (?, ?)",
                        (name, time.time()))

    def send(self, who, text):
        """DM [text] to [who]"""
        name = str(who)
        self.chat(name)
        sent = self.dms.send(self, who, text)
        now = time.time()
        self.db.execute("INSERT INTO dm_msg (ship, mine, text, at, sent, status) "
                        "VALUES (?, 1, ?, ?, ?, 'sending')", (name, text, now, sent))
        self.db.execute("UPDATE dm SET last = ? WHERE ship = ?", (now, name))
        print("-> %s: %s" % (name, text), flush=True)

    def got(self, node, msg):
        name = str(msg.ship)
        self.chat(name)
        #  INSERT OR IGNORE: messages from before tlon.DMs deduped them
        cur = self.db.execute("INSERT OR IGNORE INTO dm_msg (ship, mine, text, at, sent, "
                              "status) VALUES (?, 0, ?, ?, ?, NULL)",
                              (name, msg.text, time.time(), msg.id))
        if cur.rowcount:
            self.db.execute("UPDATE dm SET last = ?, unread = unread + 1 WHERE ship = ?",
                            (time.time(), name))
            print("<- %s: %s" % (name, msg.text), flush=True)

    def sent(self, node, her, id_, error):
        self.db.execute("UPDATE dm_msg SET status = ? WHERE ship = ? AND mine = 1 AND sent = ?",
                        ("failed" if error else "sent", str(her), id_))
        if error:
            print("!! %s refused a message: %s" % (her, " / ".join(error[:2])), flush=True)

    def on_done(self, her, bone, error):
        #  acks for messages sent on the flow this used before tlon.DMs:
        #  they come in order, so they're for the oldest still sending
        row = self.db.execute("SELECT id FROM dm_msg WHERE ship = ? AND mine = 1 AND "
                              "status = 'sending' ORDER BY id LIMIT 1",
                              (str(her),)).fetchone()
        if row:
            self.db.execute("UPDATE dm_msg SET status = ? WHERE id = ?",
                            ("failed" if error else "sent", row[0]))

    def on_tick(self):
        while not self.todo.empty():
            job = self.todo.get()
            with self.txn():
                if job[0] == "send":
                    self.send(m.ship(job[1]), job[2])
                elif job[0] == "read":
                    self.db.execute("UPDATE dm SET unread = 0 WHERE ship = ?", (job[1],))

# ---------------------------------------------------------------- the web page

PAGE = r"""<!doctype html>
<html lang="en"><head><meta charset="utf-8">
<meta name="viewport" content="width=device-width,initial-scale=1">
<title>Messages</title>
<style>
:root{--bg:#f5f4f0;--panel:#fff;--ink:#1c1b19;--mute:#77746c;--line:#e3e0d8;
--acc:#3b5bdb;--mine:#3b5bdb;--mink:#fff;--their:#ecebe6;--sel:#eceaf6}
@media (prefers-color-scheme:dark){:root{--bg:#121211;--panel:#1b1b19;--ink:#ecebe5;
--mute:#99968e;--line:#2c2b28;--acc:#8ea2ff;--mine:#3b5bdb;--mink:#fff;--their:#262624;
--sel:#23243a}}
*{box-sizing:border-box}
html,body{height:100%;margin:0}
body{background:var(--bg);color:var(--ink);font:15px/1.45 ui-sans-serif,system-ui,
-apple-system,sans-serif}
.app{height:100%;display:grid;grid-template-columns:300px 1fr;max-width:1100px;margin:0 auto;
border-left:1px solid var(--line);border-right:1px solid var(--line);background:var(--panel)}
nav{border-right:1px solid var(--line);display:flex;flex-direction:column;min-height:0}
.me{padding:16px 16px 12px;border-bottom:1px solid var(--line)}
.me h1{font-size:18px;margin:0 0 2px}
.me .id{font:12px ui-monospace,Menlo,monospace;color:var(--mute);word-break:break-all}
.new{display:flex;gap:6px;padding:10px 12px;border-bottom:1px solid var(--line)}
.new input{flex:1;min-width:0}
#chats{overflow-y:auto;flex:1}
.c{padding:11px 16px;border-bottom:1px solid var(--line);cursor:pointer;display:grid;
grid-template-columns:1fr auto;gap:2px 8px}
.c:hover{background:var(--bg)}
.c.on{background:var(--sel)}
.c .s{font:13px ui-monospace,Menlo,monospace;overflow:hidden;text-overflow:ellipsis;
white-space:nowrap}
.c .w{font-size:11.5px;color:var(--mute)}
.c .p{grid-column:1/3;font-size:13px;color:var(--mute);overflow:hidden;
text-overflow:ellipsis;white-space:nowrap}
.c .u{background:var(--acc);color:#fff;border-radius:9px;font-size:11px;padding:0 6px;
justify-self:end}
main{display:flex;flex-direction:column;min-height:0;min-width:0}
.top{padding:14px 18px;border-bottom:1px solid var(--line);display:flex;gap:10px;
align-items:center;min-height:57px}
.top .s{font:14px ui-monospace,Menlo,monospace;word-break:break-all}
.back{display:none;border:0;background:none;color:var(--acc);font:inherit;cursor:pointer;
padding:0}
.dot{width:8px;height:8px;border-radius:50%;background:#2f9e44;margin-left:auto;flex:none}
.dot.off{background:#c92a2a}
#log{flex:1;overflow-y:auto;padding:16px 18px;display:flex;flex-direction:column;gap:6px}
.b{max-width:72%;padding:8px 12px;border-radius:14px;white-space:pre-wrap;
word-break:break-word;background:var(--their);align-self:flex-start}
.b.mine{background:var(--mine);color:var(--mink);align-self:flex-end}
.meta{font-size:11px;color:var(--mute);align-self:flex-start;margin:-2px 4px 4px}
.meta.mine{align-self:flex-end}
.meta.failed{color:#c92a2a}
.hint{margin:auto;color:var(--mute);text-align:center;padding:20px}
form.say{display:flex;gap:8px;padding:12px 18px 16px;border-top:1px solid var(--line)}
input{font:inherit;padding:9px 12px;border-radius:8px;border:1px solid var(--line);
background:var(--bg);color:var(--ink)}
form.say input{flex:1}
button{font:inherit;padding:9px 14px;border-radius:8px;border:0;background:var(--acc);
color:#fff;cursor:pointer}
button:disabled{opacity:.5}
.err{color:#c92a2a;font-size:12px;padding:0 12px 8px}
@media (max-width:720px){.app{grid-template-columns:1fr}
.app.open nav{display:none}.app:not(.open) main{display:none}.back{display:block}}
</style></head><body><div class="app" id="app">
<nav><div class="me"><h1>Messages</h1><div class="id" id="me"></div></div>
<form class="new" id="nf"><input id="nw" placeholder="~sampel-palnet" autocomplete="off">
<button>New</button></form><div class="err" id="nerr"></div>
<div id="chats"></div></nav>
<main><div class="top"><button class="back" id="back">&larr;</button>
<span class="s" id="who">Pick a conversation</span><span class="dot" id="dot"></span></div>
<div id="log"><div class="hint">DMs from Tlon users show up on the left.<br>
Start one with a ship's name.</div></div>
<form class="say" id="sf"><input id="tx" placeholder="Message" autocomplete="off"
maxlength="4000" disabled><button id="go" disabled>Send</button></form></main>
</div><script>
let state={chats:[],msgs:[]},cur=null,last=0,shown=-1;
const $=id=>document.getElementById(id);
function el(t,c,x){const e=document.createElement(t);if(c)e.className=c;
if(x!=null)e.textContent=x;return e}
function when(at){const d=new Date(at*1000),n=new Date();
return d.toDateString()==n.toDateString()?d.toLocaleTimeString([],{hour:'2-digit',
minute:'2-digit'}):d.toLocaleDateString([],{month:'short',day:'numeric'})}
async function post(p,b){return fetch(p,{method:'POST',headers:{'Content-Type':
'application/json'},body:JSON.stringify(b)})}
function chats(){const box=$('chats');box.replaceChildren();
 if(!state.chats.length)box.append(el('div','hint','No conversations yet.'));
 for(const c of state.chats){const d=el('div','c'+(c.ship==cur?' on':''));
  d.append(el('span','s',c.ship),el('span','w',c.last?when(c.last):''));
  const p=el('span','p',c.preview||'');d.append(p);
  if(c.unread&&c.ship!=cur){p.style.gridColumn='1';d.append(el('span','u',c.unread))}
  d.onclick=()=>open(c.ship);box.append(d)}}
function log(){const box=$('log');if(!cur)return;
 const ms=state.msgs.filter(m=>m.ship==cur);
 if(ms.length==shown)return;
 const stick=box.scrollTop+box.clientHeight>=box.scrollHeight-40||shown<0;
 box.replaceChildren();shown=ms.length;
 if(!ms.length)box.append(el('div','hint','No messages yet. Say hello.'));
 for(const m of ms){box.append(el('div','b'+(m.mine?' mine':''),m.text));
  let meta=when(m.at);if(m.mine)meta+=m.status=='sent'?' · delivered':
  m.status=='failed'?' · not delivered':' · sending';
  box.append(el('div','meta'+(m.mine?' mine':'')+(m.status=='failed'?' failed':''),meta))}
 if(stick)box.scrollTop=box.scrollHeight}
function open(ship){cur=ship;shown=-1;$('app').classList.add('open');$('who').textContent=ship;
 $('tx').disabled=false;$('go').disabled=false;$('tx').focus();post('read',{ship});
 const c=state.chats.find(c=>c.ship==ship);if(c)c.unread=0;chats();log()}
async function poll(){try{const r=await fetch('state?after='+last,{cache:'no-store'});
 const d=await r.json();$('me').textContent=d.me;$('dot').className='dot';
 state.chats=d.chats;
 for(const m of d.msgs){const i=state.msgs.findIndex(x=>x.id==m.id);
  if(i<0)state.msgs.push(m);else state.msgs[i]=m}
 last=d.last;if(d.msgs.length)shown=-1;
 if(cur&&d.chats.find(c=>c.ship==cur&&c.unread))post('read',{ship:cur});
 chats();log()}catch(x){$('dot').className='dot off'}}
$('sf').onsubmit=async e=>{e.preventDefault();const v=$('tx').value.trim();
 if(!v||!cur)return;$('go').disabled=true;
 try{await post('send',{ship:cur,text:v});$('tx').value='';setTimeout(poll,250)}
 finally{$('go').disabled=false;$('tx').focus()}};
$('nf').onsubmit=async e=>{e.preventDefault();let v=$('nw').value.trim();
 if(!v)return;if(v[0]!='~')v='~'+v;const r=await post('check',{ship:v});
 if(!r.ok){$('nerr').textContent='not a ship name';return}
 const s=(await r.json()).ship;$('nerr').textContent='';$('nw').value='';
 if(!state.chats.find(c=>c.ship==s))state.chats.unshift({ship:s,last:0,unread:0,preview:''});
 open(s)};
$('back').onclick=()=>{cur=null;$('app').classList.remove('open');chats()};
poll();setInterval(poll,1500);
</script></body></html>
"""

def serve_web(node, db_path, addr):
    """the page, in a thread: it reads its own connection to the database
    and hands sends to the node's loop"""
    host, _, port = addr.rpartition(":")
    db = sqlite3.connect("file:%s?mode=ro" % db_path, uri=True, check_same_thread=False)
    lock = threading.Lock()
    me = str(node.our)

    class H(http.server.BaseHTTPRequestHandler):
        def reply(self, code, typ, body):
            self.send_response(code)
            self.send_header("Content-Type", typ)
            self.send_header("Content-Length", str(len(body)))
            self.send_header("Cache-Control", "no-store")
            self.end_headers()
            self.wfile.write(body)

        def json(self, obj, code=200):
            self.reply(code, "application/json", json.dumps(obj).encode())

        def do_GET(self):
            if self.path in ("/", "/index.html"):
                return self.reply(200, "text/html; charset=utf-8", PAGE.encode())
            if self.path.startswith("/state"):
                try:
                    after = int(self.path.split("after=", 1)[1])
                except (IndexError, ValueError):
                    after = 0
                with lock:
                    chats = [{"ship": s, "last": l, "unread": u,
                              "preview": (p or "")[:80]}
                             for s, l, u, p in db.execute(
                                 "SELECT d.ship, d.last, d.unread, (SELECT text FROM dm_msg "
                                 "WHERE ship = d.ship ORDER BY id DESC LIMIT 1) "
                                 "FROM dm d ORDER BY d.last DESC")]
                    #  new messages, and our recent ones again (their status changes)
                    msgs = db.execute("SELECT id, ship, mine, text, at, status FROM dm_msg "
                                      "WHERE id > ? OR (mine = 1 AND id > ? - 200) "
                                      "ORDER BY id LIMIT 2000", (after, after)).fetchall()
                    top = db.execute("SELECT max(id) FROM dm_msg").fetchone()[0] or 0
                return self.json({"me": me, "chats": chats, "last": top,
                                  "msgs": [{"id": i, "ship": s, "mine": bool(mi), "text": t,
                                            "at": a, "status": st}
                                           for i, s, mi, t, a, st in msgs]})
            self.reply(404, "text/plain", b"not found\n")

        def do_POST(self):
            try:
                n = min(int(self.headers.get("Content-Length", 0)), 32768)
                body = json.loads(self.rfile.read(n))
                who = m.ship(body["ship"].strip())
            except (ValueError, KeyError, TypeError, AttributeError):
                return self.reply(400, "text/plain", b"bad request\n")
            if who == node.our:
                return self.reply(400, "text/plain", b"that's us\n")
            if self.path == "/check":
                return self.json({"ship": str(who)})
            if self.path == "/send":
                text = str(body.get("text", "")).strip()[:4000]
                if text:
                    node.todo.put(("send", str(who), text))
                return self.reply(204, "text/plain", b"")
            if self.path == "/read":
                node.todo.put(("read", str(who)))
                return self.reply(204, "text/plain", b"")
            self.reply(404, "text/plain", b"not found\n")

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
    ap.add_argument("--web", metavar="[ADDR:]PORT", default="127.0.0.1:8474",
                    help="where to serve the page (default 127.0.0.1:8474)")
    ap.add_argument("--port", type=int, default=0,
                    help="UDP port for the network (default: any); a fixed one "
                         "keeps the address other ships know us by across restarts")
    a = ap.parse_args()

    if a.k:
        ident = m.Identity.keyfile(a.k)
    elif a.c:
        ident = m.Identity.comet(a.s + ".key")
    else:
        ident = m.Identity.fake(a.F)

    node = DMs(ident, a.s, breached=a.fresh, port=a.port)
    serve_web(node, a.s, a.web if ":" in a.web else "127.0.0.1:" + a.web)
    print("%s is online; DM it from Tlon" % node.our, flush=True)
    node.run()

if __name__ == "__main__":
    main()

// dmbot answers Tlon direct messages. Your ship (a moon, planet, star or
// comet) runs here instead of in Urbit; DM it from Tlon and it replies.
//
//	go run ./examples/dmbot -k ~sampel-palnet.key -s bot.db
//	go run ./examples/dmbot -c -s bot.db          # as a fresh comet
//
// It speaks Tlon's DM protocol (tloncorp/tlon-apps, desk/app/chat.hoon):
// it runs as agent %chat, takes %chat-dm-diff-2 pokes and sends the same,
// and answers Tlon's version check (a watch on
// /~/negotiate/version/chat-dms) with %chat-dms version 2.
//
// Each reply is sent in the same transaction that acks the message it
// answers, along with the bot's own bookkeeping: a crash at any point
// neither loses a reply nor sends one twice.
package main

import (
	"context"
	"database/sql"
	"flag"
	"fmt"
	"os"
	"os/signal"
	"strings"
	"time"

	mesa "github.com/mopfel-winrux/libmesa/bindings/go"
	_ "modernc.org/sqlite"
)

const agent = "chat"

type bot struct {
	mesa.BaseApp
	db      *sql.Conn
	started time.Time
	lastDa  time.Time
}

func (b *bot) exec(q string, args ...any) error {
	_, err := b.db.ExecContext(context.Background(), q, args...)
	return err
}

// Watch: Tlon's version check before it DMs us.
func (b *bot) Watch(n *mesa.Node, her mesa.Ship, bone uint64, ag, path string) error {
	if ag == agent && path == "/~/negotiate/version/chat-dms" {
		return n.Fact(her, bone, "noun", 2)
	}
	return mesa.Drop
}

func (b *bot) Poke(n *mesa.Node, her mesa.Ship, bone uint64, ag, mark string, noun mesa.Noun) error {
	if ag != agent {
		return mesa.Drop
	}
	if !strings.HasPrefix(mark, "chat-dm-diff") {
		return nil // rsvps, (un)blocks: nothing to do
	}
	//  [id=[author sent] %add essay=[[content author sent] kind meta blob] time]
	delta := noun.Tail()
	if delta.Head().Text() != "add" {
		return nil // deletes, reactions, replies
	}
	memo := delta.Tail().Head().Head()
	sent := memo.Tail().Tail().Big().String()
	text := strings.TrimSpace(storyText(memo.Head()))

	//  Tlon may resend a message on a new flow: its sent time is its id
	res, err := b.db.ExecContext(context.Background(),
		"INSERT OR IGNORE INTO seen (ship, sent) VALUES (?, ?)", her.String(), sent)
	if err != nil {
		return err
	}
	if k, _ := res.RowsAffected(); k == 0 {
		return nil
	}
	fmt.Printf("<- %s: %s\n", her, text)
	return b.send(n, her, b.reply(her, text))
}

func (b *bot) reply(her mesa.Ship, text string) string {
	switch strings.ToLower(text) {
	case "help", "?":
		return "I'm a bot written in Go on libmesa. Try: time, uptime, whoami, or say anything."
	case "time":
		return time.Now().UTC().Format("2006.1.2 15:04:05 UTC")
	case "uptime":
		return "up " + time.Since(b.started).Round(time.Second).String()
	case "whoami":
		return fmt.Sprintf("you are %s (a %s)", her, rankName(her.Rank()))
	}
	return "you said: " + text
}

// send DMs [text] to [her], on one flow per conversation so our messages
// arrive in order.
func (b *bot) send(n *mesa.Node, her mesa.Ship, text string) error {
	var bone uint64
	err := b.db.QueryRowContext(context.Background(),
		"SELECT bone FROM dm WHERE ship = ?", her.String()).Scan(&bone)
	if err == sql.ErrNoRows {
		if bone, err = n.OpenFlow(her); err == nil {
			err = b.exec("INSERT INTO dm (ship, bone) VALUES (?, ?)", her.String(), bone)
		}
	}
	if err != nil {
		return err
	}
	//  a fresh @da per message: it's the message's id
	now := time.Now()
	if !now.After(b.lastDa) {
		now = b.lastDa.Add(time.Millisecond)
	}
	b.lastDa = now
	sent := mesa.Da(now)
	us := n.Ship()
	essay := mesa.Tuple(mesa.Tuple(story(text), us, sent), mesa.Tuple("chat", nil), nil, nil)
	fmt.Printf("-> %s: %s\n", her, text)
	return n.Poke(her, bone, agent, "chat-dm-diff-2",
		mesa.Tuple(mesa.Tuple(us, sent), mesa.Tuple("add", essay, nil)))
}

func (b *bot) Done(n *mesa.Node, her mesa.Ship, bone uint64, err *mesa.Nack) {
	if err != nil {
		fmt.Printf("!! %s refused a message: %v\n", her, err)
	}
}

// story is plain text as a Tlon story: one paragraph, lines broken.
func story(text string) mesa.Value {
	var inl []mesa.Value
	for i, line := range strings.Split(text, "\n") {
		if i > 0 {
			inl = append(inl, mesa.Tuple("break", nil))
		}
		if line != "" {
			inl = append(inl, line)
		}
	}
	if len(inl) == 0 {
		inl = append(inl, "")
	}
	return mesa.List(mesa.Tuple("inline", mesa.List(inl...)))
}

// storyText is a story's plain text, best effort.
func storyText(s mesa.Noun) string {
	var out []string
	for _, verse := range s.List() {
		switch verse.Head().Text() {
		case "inline":
			out = append(out, inlines(verse.Tail(), 0))
		case "block":
			out = append(out, "["+verse.Tail().Head().Text()+"]")
		}
	}
	return strings.Join(out, "\n")
}

func inlines(l mesa.Noun, depth int) string {
	var b strings.Builder
	for _, i := range l.List() {
		b.WriteString(inline(i, depth))
	}
	return b.String()
}

func inline(i mesa.Noun, depth int) string {
	if depth > 32 {
		return ""
	}
	if i.IsAtom() {
		return i.Text()
	}
	p := i.Tail()
	switch i.Head().Text() {
	case "italics", "bold", "strike", "blockquote":
		return inlines(p, depth+1)
	case "inline-code", "code":
		return p.Text()
	case "tag":
		return "#" + p.Text()
	case "ship":
		b := p.Bytes()
		var s mesa.Ship
		for k := 0; k < len(b) && k < 16; k++ {
			if k < 8 {
				s.Lo |= uint64(b[k]) << (8 * k)
			} else {
				s.Hi |= uint64(b[k]) << (8 * (k - 8))
			}
		}
		return s.String()
	case "break":
		return "\n"
	case "link":
		if t := p.Tail().Text(); t != "" {
			return t
		}
		return p.Head().Text()
	}
	return ""
}

func rankName(r mesa.Rank) string {
	return [...]string{"galaxy", "star", "planet", "moon", "comet"}[r]
}

func main() {
	key := flag.String("k", "", "keyfile (planet, star, moon)")
	comet := flag.Bool("c", false, "run as a comet (keys in DB.key, mined if new)")
	fake := flag.String("F", "", "a fake ship, for `urbit -F`")
	dbPath := flag.String("s", "bot.db", "state (SQLite)")
	port := flag.Int("p", 0, "UDP port")
	flag.Parse()

	var id *mesa.Identity
	var err error
	switch {
	case *key != "":
		id, err = mesa.LoadKeyfile(*key)
	case *comet:
		id, err = mesa.LoadComet(*dbPath + ".key")
	case *fake != "":
		var s mesa.Ship
		if s, err = mesa.ParseShip(*fake); err == nil {
			id = mesa.FakeIdentity(s)
		}
	default:
		err = fmt.Errorf("one of -k, -c or -F")
	}
	if err != nil {
		fmt.Fprintln(os.Stderr, "dmbot:", err)
		os.Exit(1)
	}

	st, err := mesa.OpenSQLite("sqlite", *dbPath)
	if err != nil {
		fmt.Fprintln(os.Stderr, "dmbot:", err)
		os.Exit(1)
	}
	defer st.Close()
	for _, q := range []string{
		"CREATE TABLE IF NOT EXISTS dm (ship TEXT PRIMARY KEY, bone INTEGER NOT NULL)",
		"CREATE TABLE IF NOT EXISTS seen (ship TEXT, sent TEXT, PRIMARY KEY (ship, sent))",
	} {
		if _, err := st.Conn().ExecContext(context.Background(), q); err != nil {
			fmt.Fprintln(os.Stderr, "dmbot:", err)
			os.Exit(1)
		}
	}

	b := &bot{db: st.Conn(), started: time.Now()}
	n, err := mesa.Open(mesa.Config{Identity: id, Store: st, App: b, Port: *port})
	if err != nil {
		fmt.Fprintln(os.Stderr, "dmbot:", err)
		os.Exit(1)
	}
	defer n.Close()
	fmt.Fprintf(os.Stderr, "%s is listening for DMs on %s\n", n.Ship(), n.Addr())

	ctx, stop := signal.NotifyContext(context.Background(), os.Interrupt)
	defer stop()
	n.Run(ctx)
}

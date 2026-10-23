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
	"flag"
	"fmt"
	"os"
	"os/signal"
	"strings"
	"time"

	mesa "github.com/mopfel-winrux/libmesa/bindings/go"
	"github.com/mopfel-winrux/libmesa/bindings/go/tlon"
	_ "modernc.org/sqlite"
)

type bot struct {
	dms     tlon.DMs
	started time.Time
}

// message answers a DM, in the transaction that acks it.
func (b *bot) message(n *mesa.Node, m tlon.Message) error {
	fmt.Printf("<- %s: %s\n", m.From, m.Text)
	text := b.reply(m.From, m.Text)
	fmt.Printf("-> %s: %s\n", m.From, text)
	_, err := b.dms.Send(n, m.From, text)
	return err
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
	b := &bot{started: time.Now()}
	b.dms.OnMessage = b.message
	b.dms.OnSent = func(n *mesa.Node, her mesa.Ship, id string, err *mesa.Nack) {
		if err != nil {
			fmt.Printf("!! %s refused a message: %v\n", her, err)
		}
	}
	n, err := mesa.Open(mesa.Config{Identity: id, Store: st, Port: *port,
		Helpers: []mesa.Helper{&b.dms}})
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

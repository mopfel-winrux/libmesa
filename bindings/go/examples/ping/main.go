// ping pokes an agent on a ship and prints the ack or nack, or, with -l,
// stays online acking pokes to any agent and printing them.
//
//	go run ./examples/ping -c -s comet.db ~zod ping hi
//	go run ./examples/ping -c -s comet.db -l          # then |hi ~comet
package main

import (
	"context"
	"flag"
	"fmt"
	"os"
	"os/signal"
	"strings"

	mesa "github.com/mopfel-winrux/libmesa/bindings/go"
	_ "modernc.org/sqlite"
)

type app struct {
	mesa.BaseApp
	listen bool
	stop   func()
}

func (a *app) Poke(n *mesa.Node, her mesa.Ship, bone uint64, agent, mark string, noun mesa.Noun) error {
	if !a.listen {
		return mesa.Drop
	}
	fmt.Printf("%s poked %%%s with %%%s %s\n", her, agent, mark, noun)
	return nil
}

func (a *app) Done(n *mesa.Node, her mesa.Ship, bone uint64, err *mesa.Nack) {
	if err == nil {
		fmt.Println("ack")
	} else {
		fmt.Println("nack %" + err.Tag)
		for _, l := range err.Lines {
			fmt.Println("  " + l)
		}
	}
	if !a.listen {
		a.stop()
	}
}

func main() {
	key := flag.String("k", "", "keyfile (planet, star, moon)")
	comet := flag.Bool("c", false, "run as a comet (keys in DB.key, mined if new)")
	fake := flag.String("F", "", "a fake ship, for `urbit -F`")
	db := flag.String("s", "ship.db", "state (SQLite)")
	listen := flag.Bool("l", false, "stay online, acking and printing pokes")
	port := flag.Int("p", 0, "UDP port")
	flag.Usage = func() {
		fmt.Fprintln(os.Stderr, "usage: ping [-k key | -c | -F ship] [-s db] ~ship agent text\n       ping [-k key | -c | -F ship] [-s db] -l")
		flag.PrintDefaults()
	}
	flag.Parse()
	if *listen && flag.NArg() != 0 || !*listen && flag.NArg() < 3 {
		flag.Usage()
		os.Exit(2)
	}

	var id *mesa.Identity
	var err error
	switch {
	case *key != "":
		id, err = mesa.LoadKeyfile(*key)
	case *comet:
		id, err = mesa.LoadComet(*db + ".key")
	case *fake != "":
		var s mesa.Ship
		if s, err = mesa.ParseShip(*fake); err == nil {
			id = mesa.FakeIdentity(s)
		}
	default:
		err = fmt.Errorf("one of -k, -c or -F")
	}
	if err != nil {
		fmt.Fprintln(os.Stderr, "ping:", err)
		os.Exit(1)
	}

	st, err := mesa.OpenSQLite("sqlite", *db)
	if err != nil {
		fmt.Fprintln(os.Stderr, "ping:", err)
		os.Exit(1)
	}
	defer st.Close()

	ctx, stop := signal.NotifyContext(context.Background(), os.Interrupt)
	defer stop()
	a := &app{listen: *listen, stop: stop}
	n, err := mesa.Open(mesa.Config{Identity: id, Store: st, App: a, Port: *port})
	if err != nil {
		fmt.Fprintln(os.Stderr, "ping:", err)
		os.Exit(1)
	}
	defer n.Close()
	fmt.Fprintf(os.Stderr, "we are %s on %s\n", n.Ship(), n.Addr())

	if !*listen {
		her, err := mesa.ParseShip(flag.Arg(0))
		if err == nil {
			err = n.Do(func() error {
				bone, err := n.OpenFlow(her)
				if err != nil {
					return err
				}
				return n.Poke(her, bone, flag.Arg(1), "noun", strings.Join(flag.Args()[2:], " "))
			})
		}
		if err != nil {
			fmt.Fprintln(os.Stderr, "ping:", err)
			os.Exit(1)
		}
	}
	n.Run(ctx)
}

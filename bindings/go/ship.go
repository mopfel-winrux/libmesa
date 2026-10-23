package mesa

// #include "shim.h"
import "C"

import (
	"crypto/rand"
	"errors"
	"fmt"
	"io"
	"net/http"
	"os"
	"strings"
	"time"
	"unsafe"
)

// Ship is an Urbit ship: a 128-bit number, rendered as @p.
type Ship struct{ Lo, Hi uint64 }

// Rank is a ship's class.
type Rank int

const (
	Galaxy Rank = iota // 8 bits
	Star               // 16 bits
	Planet             // 32 bits
	Moon               // 64 bits
	Comet              // 128 bits
)

func (s Ship) c() C.mesa_ship { return C.mesa_ship{lo: C.uint64_t(s.Lo), hi: C.uint64_t(s.Hi)} }

func shipOf(c *C.mesa_ship) Ship { return Ship{uint64(c.lo), uint64(c.hi)} }

// ParseShip reads "~sampel-palnet" (the ~ is optional).
func ParseShip(text string) (Ship, error) {
	if !strings.HasPrefix(text, "~") {
		text = "~" + text
	}
	cs := C.CString(text)
	defer C.free(unsafe.Pointer(cs))
	var c C.mesa_ship
	if !C.mesa_ship_parse(cs, &c) {
		return Ship{}, fmt.Errorf("mesa: not a ship: %q", text)
	}
	return shipOf(&c), nil
}

// MustShip is ParseShip for constants; it panics on a bad name.
func MustShip(text string) Ship {
	s, err := ParseShip(text)
	if err != nil {
		panic(err)
	}
	return s
}

// String is the ship's @p, e.g. "~zod".
func (s Ship) String() string {
	p := C.mesa_ship_render(s.c())
	defer C.mesa_free(unsafe.Pointer(p))
	return C.GoString(p)
}

func (s Ship) Rank() Rank { return Rank(C.mesa_ship_rank(s.c())) }

// Sponsor is the ship's default sponsor (+sein): a moon's planet, a
// planet's star, a star's galaxy, a comet's star.
func (s Ship) Sponsor() Ship { c := C.mesa_ship_sein(s.c()); return shipOf(&c) }

// Identity is who we are: a ship and the keys that prove it.
type Identity struct {
	c    C.mesa_ident
	fake bool
}

// Ship is the identity's ship.
func (id *Identity) Ship() Ship { return shipOf(&id.c.who) }

// Life is the identity's key revision.
func (id *Identity) Life() uint64 { return uint64(id.c.life) }

// Fake reports whether this is a fake ship (`urbit -F`).
func (id *Identity) Fake() bool { return id.fake }

// FakeIdentity is a fake ship, whose keys derive from its name; only
// other fake ships (`urbit -F`) will talk to it.
func FakeIdentity(who Ship) *Identity {
	id := &Identity{fake: true}
	C.mesa_ident_fake(who.c(), &id.c)
	return id
}

// NewIdentity is [who] at [life] and [rift] with the suite %b [ring]: 'B'
// and 64 bytes of seed. A moon's parent registers the moon with only its
// Pass, so a moon can make its own keys this way and keep them secret.
func NewIdentity(who Ship, life, rift uint64, ring [65]byte) *Identity {
	id := &Identity{}
	id.c.who = who.c()
	id.c.life, id.c.rift = C.uint64_t(life), C.uint64_t(rift)
	for i, b := range ring {
		id.c.ring[i] = C.uint8_t(b)
	}
	return id
}

// Pass is the identity's public key: 'b' ++ sgn ++ cry, 65 bytes,
// little-endian as an atom (the pass jael and azimuth hold).
func (id *Identity) Pass() [65]byte {
	var p [65]byte
	C.mesa_ident_pass(&id.c, (*C.uint8_t)(unsafe.Pointer(&p[0])))
	return p
}

// ParseKeyfile reads a keyfile: what `urbit -k` takes, or what a
// parent's |moon prints.
func ParseKeyfile(text string) (*Identity, error) {
	cs := C.CString(strings.TrimSpace(text))
	defer C.free(unsafe.Pointer(cs))
	id := &Identity{}
	if C.mesa_keyfile_parse(cs, &id.c, 1) < 1 {
		return nil, errors.New("mesa: not a keyfile")
	}
	return id, nil
}

// LoadKeyfile reads a keyfile from path.
func LoadKeyfile(path string) (*Identity, error) {
	b, err := os.ReadFile(path)
	if err != nil {
		return nil, err
	}
	id, err := ParseKeyfile(string(b))
	if err != nil {
		return nil, fmt.Errorf("%s: %w", path, err)
	}
	return id, nil
}

// Keyfile renders the identity as a keyfile.
func (id *Identity) Keyfile() string {
	p := C.mesa_keyfile_render(&id.c)
	defer C.mesa_free(unsafe.Pointer(p))
	return C.GoString(p)
}

// CometStarsURL lists the stars a comet may be mined under.
const CometStarsURL = "https://bootstrap.urbit.org/comet-stars.jam"

// LoadComet loads the comet whose keyfile is at path, or mines a new one
// and saves it there (owner-only; the keys are secret).
func LoadComet(path string) (*Identity, error) {
	if _, err := os.Stat(path); err == nil {
		return LoadKeyfile(path)
	}
	stars, err := cometStars()
	if err != nil {
		return nil, err
	}
	id, err := MineComet(stars)
	if err != nil {
		return nil, err
	}
	f, err := os.OpenFile(path, os.O_WRONLY|os.O_CREATE|os.O_EXCL, 0o600)
	if err != nil {
		return nil, err
	}
	if _, err = f.WriteString(id.Keyfile() + "\n"); err == nil {
		err = f.Close()
	} else {
		f.Close()
	}
	return id, err
}

// MineComet mines a comet under one of stars (+come:dawn).
func MineComet(stars []Ship) (*Identity, error) {
	if len(stars) == 0 {
		return nil, errors.New("mesa: no stars to mine a comet under")
	}
	cs := make([]C.mesa_ship, len(stars))
	for i, s := range stars {
		cs[i] = s.c()
	}
	eny := make([]byte, 64)
	if _, err := rand.Read(eny); err != nil {
		return nil, err
	}
	id := &Identity{}
	if !C.mesa_comet_mine(C.size_t(len(cs)), &cs[0], (*C.uint8_t)(unsafe.Pointer(&eny[0])),
		C.uint64_t(^uint64(0)), &id.c) {
		return nil, errors.New("mesa: comet mining failed")
	}
	return id, nil
}

func cometStars() ([]Ship, error) {
	cl := &http.Client{Timeout: 30 * time.Second}
	r, err := cl.Get(CometStarsURL)
	if err != nil {
		return nil, err
	}
	defer r.Body.Close()
	jam, err := io.ReadAll(r.Body)
	if err != nil || len(jam) == 0 {
		return nil, fmt.Errorf("mesa: comet-stars: %v", err)
	}
	lis := C.mesa_cue_bytes(C.size_t(len(jam)), (*C.uint8_t)(unsafe.Pointer(&jam[0])))
	if lis == none {
		return nil, errors.New("mesa: bad comet-stars.jam")
	}
	defer C.mesa_unref(lis)
	var stars []Ship
	for _, s := range (Noun{lis}).List() {
		v, _ := s.Uint64()
		stars = append(stars, Ship{Lo: v})
	}
	return stars, nil
}

// Point is what the network knows about a ship: its keys, continuity
// and sponsor.
type Point struct {
	Ship       Ship
	Rift       uint64
	Life       uint64 // 0: no keys yet
	Pass       [65]byte
	HasSponsor bool
	Sponsor    Ship
}

func pointOf(c *C.mesa_point) *Point {
	p := &Point{
		Ship:       shipOf(&c.who),
		Rift:       uint64(c.rift),
		Life:       uint64(c.life),
		HasSponsor: bool(c.has_sponsor),
		Sponsor:    shipOf(&c.sponsor),
	}
	for i := range p.Pass {
		p.Pass[i] = byte(c.pass[i])
	}
	return p
}

func (p *Point) c() C.mesa_point {
	c := C.mesa_point{
		who:         p.Ship.c(),
		rift:        C.uint64_t(p.Rift),
		life:        C.uint64_t(p.Life),
		has_sponsor: C.bool(p.HasSponsor),
		sponsor:     p.Sponsor.c(),
	}
	for i := range p.Pass {
		c.pass[i] = C.uint8_t(p.Pass[i])
	}
	return c
}

// AzimuthURL is the default azimuth-rpc endpoint for ships' keys.
const AzimuthURL = "https://roller.urbit.org/v1/azimuth"

// ErrNotFound: the ship isn't on azimuth (not spawned).
var ErrNotFound = errors.New("mesa: not on azimuth")

// FetchPoint looks a ship up on azimuth (getPoint). It blocks; it is
// safe from any goroutine.
func FetchPoint(cl *http.Client, url string, who Ship) (*Point, error) {
	req := C.mesa_azimuth_point_request(who.c())
	body := C.GoString(req)
	C.mesa_free(unsafe.Pointer(req))

	r, err := cl.Post(url, "application/json", strings.NewReader(body))
	if err != nil {
		return nil, err
	}
	defer r.Body.Close()
	js, err := io.ReadAll(r.Body)
	if err != nil {
		return nil, err
	}
	if len(js) == 0 {
		return nil, errors.New("mesa: empty azimuth response")
	}
	var c C.mesa_point
	switch C.mesa_azimuth_point_parse(who.c(), C.size_t(len(js)),
		(*C.char)(unsafe.Pointer(&js[0])), &c) {
	case C.MESA_AZIMUTH_OK:
		return pointOf(&c), nil
	case C.MESA_AZIMUTH_NOT_FOUND:
		return nil, ErrNotFound
	}
	return nil, fmt.Errorf("mesa: bad azimuth response for %s", who)
}

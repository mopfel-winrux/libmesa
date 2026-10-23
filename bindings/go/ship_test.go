package mesa

import (
	"crypto/rand"
	"testing"
)

// Keys made with NewIdentity survive a keyfile, and a fake ship's keys
// are what FakeIdentity derives.
func TestNewIdentity(t *testing.T) {
	moon := MustShip("~doprut-posfel-dozzod-dozzod")
	var ring [65]byte
	ring[0] = 'B'
	rand.Read(ring[1:])
	id := NewIdentity(moon, 1, 0, ring)
	if id.Ship() != moon || id.Life() != 1 {
		t.Fatalf("got %s life %d", id.Ship(), id.Life())
	}
	back, err := ParseKeyfile(id.Keyfile())
	if err != nil {
		t.Fatal(err)
	}
	if back.Ship() != moon || back.Pass() != id.Pass() {
		t.Fatal("keyfile round trip changed the keys")
	}
	if p := id.Pass(); p[0] != 'b' {
		t.Fatalf("pass starts %q", p[0])
	}
	if FakeIdentity(moon).Pass() == id.Pass() {
		t.Fatal("random keys equal the fake ones")
	}
}

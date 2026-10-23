//go:build windows

package mesa

import (
	"os"
	"syscall"
	"unsafe"
)

var lockFileEx = syscall.NewLazyDLL("kernel32.dll").NewProc("LockFileEx")

const (
	lockfileFailImmediately = 0x1
	lockfileExclusiveLock   = 0x2
)

// lockFile takes an exclusive lock on f without waiting, as libmesa's
// C store does on the same file (LockFileEx on its first byte).
func lockFile(f *os.File) error {
	var ov syscall.Overlapped
	r, _, err := lockFileEx.Call(f.Fd(), lockfileExclusiveLock|lockfileFailImmediately,
		0, 1, 0, uintptr(unsafe.Pointer(&ov)))
	if r == 0 {
		return err
	}
	return nil
}

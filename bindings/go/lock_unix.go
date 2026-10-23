//go:build !windows

package mesa

import (
	"os"
	"syscall"
)

// lockFile takes an exclusive lock on f without waiting, as libmesa's
// C store does on the same file.
func lockFile(f *os.File) error {
	return syscall.Flock(int(f.Fd()), syscall.LOCK_EX|syscall.LOCK_NB)
}

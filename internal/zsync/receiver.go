package zsync

// AI: copilot / grok code fast conversion of the receiver parts of zsync's zsync.c.

import (
	"github.com/cph/zsync/internal/rcksum"
)

// ZsyncReceiver handles receiving data for a zsync download
type ZsyncReceiver struct {
	Zs        *State
	UrlType   int
	Outbuf    []byte
	Outoffset int64
}

// ZsyncBeginReceive starts receiving data
func BeginReceive(zs *State, urlType int) *ZsyncReceiver {
	if urlType != 0 {
		return nil
	}
	return &ZsyncReceiver{
		Zs:        zs,
		UrlType:   urlType,
		Outbuf:    make([]byte, zs.blocksize),
		Outoffset: 0,
	}
}

// ReceiveData receives data
func ReceiveData(zr *ZsyncReceiver, buf []byte, offset, length int64) int {
	ret := 0
	blocksize := zr.Zs.blocksize
	buf = buf[:length]

	if offset%blocksize != 0 {
		x := length
		if x > blocksize-(offset%blocksize) {
			x = blocksize - (offset % blocksize)
		}

		if zr.Outoffset == offset {
			copy(zr.Outbuf[offset%blocksize:], buf[:x])
			if (x+offset)%blocksize == 0 {
				if zr.Zs.Rs.SubmitBlocks(zr.Outbuf, rcksum.BlockID((zr.Outoffset+x-blocksize)/blocksize), rcksum.BlockID((zr.Outoffset+x-blocksize)/blocksize)) != 0 {
					ret = 1
				}
			}
		}
		buf = buf[x:]
		offset += x
	}

	if len(buf) >= int(blocksize) {
		w := len(buf) / int(blocksize)
		start := rcksum.BlockID(offset / blocksize)
		end := start + rcksum.BlockID(w) - 1
		if zr.Zs.Rs.SubmitBlocks(buf[:w*int(blocksize)], start, end) != 0 {
			ret = 1
		}
		buf = buf[w*int(blocksize):]
		offset += int64(w) * blocksize
	}

	if len(buf) > 0 {
		copy(zr.Outbuf, buf)
		offset += int64(len(buf))
	}

	zr.Outoffset = offset
	return ret
}

// EndReceive ends receiving
func EndReceive(zr *ZsyncReceiver) {
	// Nothing to do
}

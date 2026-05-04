package zsync

// AI: copilot / grok code fast conversion of the state parts of zsync's zsync.c.

import (
	"bufio"
	"crypto/sha1"
	"encoding/binary"
	"encoding/hex"
	"fmt"
	"io"
	"os"
	"strconv"
	"strings"
	"time"

	"github.com/cph6/zsync/internal/rcksum"
)

// State holds the state of a zsync download
type State struct {
	Rs             *rcksum.RcksumState
	filelen        int64
	blocks         int64
	blocksize      int64
	checksum       string
	checksumMethod string
	urls           []string
	filename       string
	mtime          time.Time
	curFilename    string
}

// ZsyncBegin loads a zsync file and returns the state
func Begin(f *os.File) (*State, error) {
	checksumBytes := 16
	rsumBytes := 4
	seqMatches := 1

	zs := &State{}

	safelines := []string{}

	scanner := bufio.NewScanner(f)
	for scanner.Scan() {
		line := strings.TrimSpace(scanner.Text())
		if line == "" {
			break
		}
		parts := strings.SplitN(line, ": ", 2)
		if len(parts) != 2 {
			return nil, fmt.Errorf("bad line: %s", line)
		}
		key, value := parts[0], parts[1]

		switch key {
		case "zsync":
			if value == "0.0.4" {
				return nil, fmt.Errorf("not compatible with zsync 0.0.4")
			}
		case "Min-Version":
			// Assume version is ok
		case "Length":
			var err error
			zs.filelen, err = strconv.ParseInt(value, 10, 64)
			if err != nil {
				return nil, fmt.Errorf("bad length: %s", value)
			}
		case "Filename":
			zs.filename = value
		case "URL":
			zs.urls = append(zs.urls, value)
		case "Blocksize":
			blocksize, err := strconv.ParseInt(value, 10, 64)
			if err != nil {
				return nil, fmt.Errorf("bad blocksize: %s", value)
			}
			if blocksize&(blocksize-1) != 0 {
				return nil, fmt.Errorf("nonsensical blocksize %d", blocksize)
			}
			zs.blocksize = blocksize
		case "Hash-Lengths":
			parts := strings.Split(value, ",")
			if len(parts) != 3 {
				return nil, fmt.Errorf("bad hash lengths: %s", value)
			}
			var err error
			seqMatches, err = strconv.Atoi(parts[0])
			if err != nil {
				return nil, fmt.Errorf("bad seqMatches: %s", parts[0])
			}
			rsumBytes, err = strconv.Atoi(parts[1])
			if err != nil {
				return nil, fmt.Errorf("bad rsumBytes: %s", parts[1])
			}
			checksumBytes, err = strconv.Atoi(parts[2])
			if err != nil {
				return nil, fmt.Errorf("bad checksumBytes: %s", parts[2])
			}
			if rsumBytes < 1 || rsumBytes > 4 || checksumBytes < 3 || checksumBytes > 16 || seqMatches < 1 || seqMatches > 2 {
				return nil, fmt.Errorf("nonsensical hash lengths: %s", value)
			}
		case "Safe":
			safelines = append(safelines, value)
		case "SHA-1":
			if len(value) != sha1.Size*2 {
				return nil, fmt.Errorf("SHA-1 digest wrong length")
			}
			zs.checksum = value
			zs.checksumMethod = "SHA-1"
		case "MTime":
			t, err := time.Parse(time.RFC1123Z, value)
			if err != nil {
				return nil, fmt.Errorf("bad mtime: %s", value)
			}
			zs.mtime = t
		default:
			// Ignore headers if they were included in "Safe".
			for _, safe := range safelines {
				if safe == key {
					break
				}
			}
			// Otherwise reject unknown header.
			return nil, fmt.Errorf("unknown header: %s", key)
		}
	}
	if err := scanner.Err(); err != nil {
		return nil, err
	}

	if zs.filelen != 0 && zs.blocksize != 0 {
		zs.blocks = (zs.filelen + zs.blocksize - 1) / zs.blocksize
	}

	if zs.filelen == 0 || zs.blocksize == 0 {
		return nil, fmt.Errorf("not a zsync file")
	}

	rs, err := rcksum.New(rcksum.BlockID(zs.blocks), zs.blocksize, int(rsumBytes), uint(checksumBytes), seqMatches)
	if err != nil {
		return nil, err
	}
	zs.Rs = rs

	// Read block checksums
	for i := int64(0); i < zs.blocks; i++ {
		var r rcksum.RSum
		checksum := make([]byte, checksumBytes)

		// Read rsum
		if err := binary.Read(f, binary.BigEndian, r); err != nil {
			return nil, err
		}

		// Read checksum
		if _, err := io.ReadFull(f, checksum); err != nil {
			return nil, err
		}

		var cksum [rcksum.ChecksumSize]byte
		copy(cksum[:], checksum)
		rs.AddTargetBlock(rcksum.BlockID(i), r, cksum)
	}

	return zs, nil
}

// Filename returns the suggested filename for the file being reconstructed.
func Filename(zs *State) string {
	return zs.filename
}

// Mtime returns the mtime that the file should have when complete,
// or the zero time if not specified.
func Mtime(zs *State) time.Time {
	return zs.mtime
}

// Status returns:
//
//	0 if no blocks have been matched or retrieved,
//	1 if some blocks have been matched or retrieved,
//	2 if the file is complete.
func Status(zs *State) int {
	todo := zs.Rs.BlocksTodo()
	if todo == zs.blocks {
		return 0
	}
	if todo > 0 {
		return 1
	}
	return 2
}

// Progress returns the number of bytes obtained for the target file, and the total needed.
func Progress(zs *State) (got, total int64) {
	todo := zs.Rs.BlocksTodo()
	got = int64(zs.blocks-todo) * zs.blocksize
	total = int64(zs.blocks) * zs.blocksize
	return
}

// GetUrls returns the URLs from which the file can be downloaded.
func GetUrls(zs *State) []string {
	return zs.urls
}

type ByteRange struct {
	Start int64
	End   int64
}

// NeededByteRanges returns the byte ranges of the target file that still need to be obtained.
func NeededByteRanges(zs *State) []ByteRange {
	blockRanges := zs.Rs.NeededBlockRanges(0, rcksum.BlockID(zs.blocks-1))
	byteRanges := make([]ByteRange, len(blockRanges))
	for i, br := range blockRanges {
		byteRanges[i].Start = int64(br.Start) * zs.blocksize
		end := int64(br.End+1)*zs.blocksize - 1
		if end >= zs.filelen {
			end = zs.filelen - 1
		}
		byteRanges[i].End = end
	}
	return byteRanges
}

// SubmitSourceFile submits a file as a source for data for reconstructing the
// target file.
func SubmitSourceFile(zs *State, f *os.File, progress bool) error {
	_, err := zs.Rs.SubmitSourceFile(f)
	return err
}

// RenameFile renames the file in which the target file is being reconstructed.
func RenameFile(zs *State, filename string) error {
	cur := zs.currentFilename()
	if err := os.Rename(cur, filename); err != nil {
		return err
	}
	zs.curFilename = filename
	return nil
}

func (zs *State) currentFilename() string {
	if zs.curFilename == "" {
		zs.curFilename = zs.Rs.Filename()
	}
	return zs.curFilename
}

// Complete checks that the reconstructed file is complete,
// truncating the file to the correct length and verifying the checksum if it
// was provided. It returns an error if any of these checks fail.
// After this call, it is no longer valid to call other methods on the State except for End.
func Complete(zs *State) error {
	filename := zs.currentFilename()
	zs.Rs.Close()

	if zs.Rs.BlocksTodo() < zs.blocks {
		return fmt.Errorf("file is not complete")
	}
	if err := os.Truncate(filename, zs.filelen); err != nil {
		return fmt.Errorf("failed to truncate file: %w", err)
	}
	f, err := os.Open(filename)
	if err != nil {
		return fmt.Errorf("failed to open file to check checksum: %w", err)
	}
	defer f.Close()

	if zs.checksum != "" && zs.checksumMethod == "SHA-1" {
		h := sha1.New()
		if _, err := io.Copy(h, f); err != nil {
			return fmt.Errorf("failed to read file: %w", err)
		}
		digest := hex.EncodeToString(h.Sum(nil))
		if strings.EqualFold(digest, zs.checksum) {
			return nil
		}
		return fmt.Errorf("checksum mismatch")
	}
	return nil
}

// End cleans up resources held by the State and returns the filename of the
// reconstructed file. The file returned is the fully reconstructed and
// verified target file if and only if the caller has previously called
// Complete() and it returned nil.
// End can be called without Complete() in the event that file reconstruction
// is abandoned; in that case the returned filename is the temporary file with
// partial data, which can be submitted as a source file for a future zsync
// run of this or a successor version of the target file.
func End(zs *State) string {
	filename := zs.currentFilename()
	if zs.Rs != nil {
		zs.Rs.Close()
	}
	return filename
}

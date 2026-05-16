package zsync

/*
 * SPDX-FileCopyrightText: 2004,2005,2007,2009,2025,2026 Colin Phipps <cph@moria.org.uk>
 *
 * SPDX-License-Identifier: Artistic-2.0
 */

// AI: copilot / grok code fast conversion of the state parts of zsync's zsync.c.

// Partially thread unsafe; methods used during setup and completion of
// downloads should be used from a single thread. But methods used during data
// transfer are thread safe.
// TODO that's bad design, leftover from the single-threaded C implementation.
// So this file in particular is probably a bad abstraction.

import (
	"crypto/sha1"
	"encoding/hex"
	"fmt"
	"io"
	"os"
	"path/filepath"
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
	tempFile       *os.File
	curFilename    string
	targetFilename string
	mtime          time.Time
}

// Prepare make the State object ready for file reconstruction.
func (zs *State) Prepare(targetFilename string) error {
	// Create temporary file in the target directory.
	targetDir := filepath.Dir(targetFilename)
	tempFile, err := os.CreateTemp(targetDir, "rcksum-*")
	if err != nil {
		return fmt.Errorf("failed to create temporary file in %s: %w", targetDir, err)
	}
	zs.tempFile = tempFile
	zs.curFilename = zs.tempFile.Name()
	zs.Rs.SetTargetFile(zs.tempFile)
	return nil
}

// Filename returns the suggested filename for the file being reconstructed.
func (zs *State) Filename() string {
	return zs.targetFilename
}

// Mtime returns the mtime that the file should have when complete,
// or the zero time if not specified.
func (zs *State) Mtime() time.Time {
	return zs.mtime
}

// Status returns:
//
//	0 if no blocks have been matched or retrieved,
//	1 if some blocks have been matched or retrieved,
//	2 if the file is complete.
func (zs *State) Status() int {
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
func (zs *State) Progress() (got, total int64) {
	todo := zs.Rs.BlocksTodo()
	got = int64(zs.blocks-todo) * zs.blocksize
	total = int64(zs.blocks) * zs.blocksize
	return
}

// GetUrls returns the URLs from which the file can be downloaded.
func (zs *State) GetUrls() []string {
	return zs.urls
}

type ByteRange struct {
	Start int64
	End   int64
}

// NeededByteRanges returns the byte ranges of the target file that still need to be obtained.
func (zs *State) NeededByteRanges() []ByteRange {
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

func (zs *State) SubmitTargetData(offset int64, in io.Reader) (int64, error) {
	bytesReceived := int64(0)

	if offset%zs.blocksize != 0 {
		panic(fmt.Sprintf("misaligned data block passed as target data (%d, blocksize %d)", offset, zs.blocksize))
	}
	id := rcksum.BlockID(offset / zs.blocksize)

	buf := make([]byte, zs.blocksize)
	var n int
	var err error
	for {
		n, err = io.ReadFull(in, buf)
		if err != nil {
			break
		}
		bytesReceived += int64(n)
		// err == nil implies a full buffer.
		zs.Rs.SubmitBlocks(buf, id, id)
		id++
	}
	if err == io.EOF {
		return bytesReceived, nil
	} else if err == io.ErrUnexpectedEOF {
		if id == rcksum.BlockID(zs.blocks-1) {
			// Short last block. rcksum expects a full block, padded with 0s, so pad and submit.
			for i := range buf {
				if i >= n {
					buf[i] = 0
				}
			}
			zs.Rs.SubmitBlocks(buf, id, id)
			return bytesReceived, nil
		} // else fall through; any other incomplete block is an error.
	}
	return bytesReceived, err
}

// SubmitSourceFile submits a file as a source for data for reconstructing the
// target file.
func (zs *State) SubmitSourceFile(f *os.File, progress bool) error {
	_, err := zs.Rs.SubmitSourceFile(f, progress)
	return err
}

// RenameFile renames the file in which the target file is being reconstructed.
func (zs *State) RenameFile(filename string) error {
	cur := zs.curFilename
	if err := os.Rename(cur, filename); err != nil {
		return err
	}
	zs.curFilename = filename
	return nil
}

// Complete checks that the reconstructed file is complete,
// truncating the file to the correct length and verifying the checksum if it
// was provided. It returns an error if any of these checks fail.
// After this call, it is no longer valid to call other methods on the State except for End.
func (zs *State) Complete() error {
	if zs.Rs.BlocksTodo() > 0 {
		return fmt.Errorf("file is not complete")
	}
	zs.Rs = nil
	if err := zs.tempFile.Truncate(zs.filelen); err != nil {
		return fmt.Errorf("failed to truncate file: %w", err)
	}
	if _, err := zs.tempFile.Seek(0, io.SeekStart); err != nil {
		return fmt.Errorf("failed to seek to start of temporary file: %w", err)
	}

	if zs.checksum != "" && zs.checksumMethod == "SHA-1" {
		h := sha1.New()
		if _, err := io.Copy(h, zs.tempFile); err != nil {
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
	zs.tempFile.Close()
	return zs.curFilename
}

// Returns stats on the file reconstruction process.
// Not wrapping this in a "package zsync" version of the object here, but I may
// change that later. This is mostly debugging stats, I don't suggest that
// anyone else use it as is.
func (zs *State) Stats() rcksum.Stats {
	return zs.Rs.Stats()
}

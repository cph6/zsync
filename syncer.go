// Package zsync implements the high level process of
// loading and using a zsync control file to reconstruct a
// target file.
//
// Typically it is used with the following steps:
//   - create a Syncer with New, passing in a zsync control file.
//   - create SeedReaders and feed local data sources (older versions,
//     previous partial downloads) to them.
//   - call FetchRemainingBlocks to cause the Syncer to download and fill
//     in missing data.
//   - call Complete to verify the reconstructed file.
//   - call End to close the file and optionally move it to the final
//     filename.
package zsync

/*
 * SPDX-FileCopyrightText: 2004,2005,2007,2009,2025,2026 Colin Phipps <cph@moria.org.uk>
 *
 * SPDX-License-Identifier: Artistic-2.0
 */

// AI: copilot / grok code fast conversion of the state parts of zsync's zsync.c.

import (
	"crypto/sha1"
	"encoding/hex"
	"fmt"
	"io"
	"os"
	"strings"
	"time"

	"github.com/cph6/zsync/internal/rcksum"
)

// Syncer holds the state of the process of reconstructing and
// downloading a target file specified by a zsync control file.
// Syncer is not thread safe, except for:
// Status() and Progress() can be called while the syncer is active.
type Syncer struct {
	rs             *rcksum.RcksumState
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

// TargetFilename returns the suggested filename for the file being reconstructed.
func (zs *Syncer) TargetFilename() string {
	return zs.targetFilename
}

// Filename returns the current filename if any of the temporary file where the
// target is being reconstructed.
func (zs *Syncer) Filename() string {
	return zs.curFilename
}

const (
	NoData = iota
	PartialData
	CompleteData
)

// Status returns the current status of the reconstruction — it's a coarser
// progress indication usually used to check whether the Syncer thinks it is
// done or not. In particular the caller might want to check that Status() !=
// NO_DATA after seed files are read, if the caller wants to be sure that local
// data was in fact used.
func (zs *Syncer) Status() int {
	todo := zs.rs.BlocksTodo()
	if todo == zs.blocks {
		return NoData
	}
	if todo > 0 {
		return PartialData
	}
	return CompleteData
}

// Progress returns the number of bytes obtained for the target file, and the total needed.
func (zs *Syncer) Progress() (got, total int64) {
	todo := zs.rs.BlocksTodo()
	got = int64(zs.blocks-todo) * zs.blocksize
	total = int64(zs.blocks) * zs.blocksize
	return
}

type byteRange struct {
	Start int64
	End   int64
}

// NeededByteRanges returns the byte ranges of the target file that still need
// to be obtained.
// Public for advanced use only: this is mainly used internally, but can be
// used by the caller if desired.
func (zs *Syncer) NeededByteRanges() []byteRange {
	blockRanges := zs.rs.NeededBlockRanges(0, rcksum.BlockID(zs.blocks-1))
	byteRanges := make([]byteRange, len(blockRanges))
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

// SubmitTargetData takes received blocks of target data and uses them to fill
// in missing chunks of the target file.
// Public for advanced use only: most callers should use FetchRemainingBlocks
// instead and let the Syncer fetch and submit the blocks. This is provided for
// callers that want to handle data downloading via another protocol or
// approach feed the syncer themselves.
func (zs *Syncer) SubmitTargetData(offset int64, in io.Reader) (int64, error) {
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
		err = zs.rs.SubmitBlocks(buf, id, id)
		if err != nil {
			return bytesReceived, err
		}
		id++
	}
	switch err {
	case io.EOF:
		return bytesReceived, nil
	case io.ErrUnexpectedEOF:
		if id == rcksum.BlockID(zs.blocks-1) {
			// Short last block. rcksum expects a full block, padded with 0s, so pad and submit.
			for i := range buf {
				if i >= n {
					buf[i] = 0
				}
			}
			err = zs.rs.SubmitBlocks(buf, id, id)
			return bytesReceived, err
		} // else fall through; any other incomplete block is an error.
	default:
		// Any other error is returned as-is
	}
	return bytesReceived, err
}

// NewSeedSink returns a io.ReaderFrom that can be used to stream local seed
// local seed data to the Syncer. progressCallback, if provided, will be
// periodically invoked with the number of bytes accepted.
func (zs *Syncer) NewSeedSink(progressCallback func(int64)) io.ReaderFrom {
	return zs.rs.NewSeedSink(progressCallback)
}

// RenameFile renames the file in which the target file is being reconstructed.
func (zs *Syncer) RenameFile(filename string) error {
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
// After this call, it is no longer valid to call other methods on the Syncer except for End.
func (zs *Syncer) Complete() error {
	if zs.rs.BlocksTodo() > 0 {
		return fmt.Errorf("file is not complete")
	}
	zs.rs = nil
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

// End cleans up resources held by the Syncer and returns the filename of the
// reconstructed file. The file returned is the fully reconstructed and
// verified target file if and only if the caller has previously called
// Complete() and it returned nil.
// End can be called without Complete() in the event that file reconstruction
// is abandoned; in that case the returned filename is the temporary file with
// partial data, which can be submitted as a source file for a future zsync
// run of this or a successor version of the target file.
func (zs *Syncer) End(targetFilename string) (string, error) {
	err := zs.tempFile.Close()
	if err != nil {
		return zs.curFilename, err
	}
	if targetFilename != "" {
		if err := os.Rename(zs.curFilename, targetFilename); err != nil {
			return zs.curFilename, fmt.Errorf("unable to move %s to %s: %v", zs.curFilename, targetFilename, err)
		}
		zs.curFilename = targetFilename
	}
	if !zs.mtime.IsZero() {
		if err := os.Chtimes(zs.curFilename, time.Now(), zs.mtime); err != nil {
			fmt.Fprintf(os.Stderr, "warning: failed to set mtime: %v\n", err)
		}
	}
	return zs.curFilename, nil
}

// RStats returns stats on the rsync/rolling checksum part of the file
// reconstruction process. This is mostly debugging stats — I don't
// suggest that anyone else use it as is.
func (zs *Syncer) RStats() rcksum.Stats {
	return zs.rs.Stats()
}

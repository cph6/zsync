package rcksum

/*
 * SPDX-FileCopyrightText: 2004,2005,2007,2009,2025,2026 Colin Phipps <cph@moria.org.uk>
 *
 * SPDX-License-Identifier: Artistic-2.0
 */

// AI: copilot / claude code (if I remember correctly) conversion of zsync's hash.c.

import (
	"bytes"
	"fmt"
	"io"
)

// SeedSink holds the state when reading a stream of seed data.
// Currently this implemenets io.ReaderFrom, though it could implement
// io.ReadCloser to if there was a need.
// Note that while you can use this for submitting target data, it is not
// optimal to do so (we know which blocks target data matches so there is no
// need to apply the rolling checksum method that SeedSink uses) —  use
// SubmitBlocks instead.
type SeedSink struct {
	z       *RcksumState
	context int // minimum context needed

	buf       []byte
	bufOffset int     // current position in `buf`
	bufLen    int     // buf[0:bufLen] is currently filled with data.
	r         [2]RSum // rolling checksums at that position, if calculated.
	rInvalid  bool

	BlocksMatched int

	progressCallback func(offset int64)
}

// NewSeedSink initializes a SeedSink for reading a stream of seed data.
// Args:
//   - progressCallback: a function called at intervals with the current read
//     offset in the stream, giving the caller an opportunity to print progress
//     information.
func (z *RcksumState) NewSeedSink(progressCallback func(int64)) *SeedSink {
	bufSize := int(z.blockSize) * 16
	return &SeedSink{
		z:                z,
		buf:              make([]byte, bufSize),
		context:          int(z.blockSize) * z.seqMatches,
		rInvalid:         true,
		progressCallback: progressCallback,
	}
}

// scan searches currently buffered data for target blocks and, if
// found, writes them to the target file.
// This function expects certain state to be conserved in the SeedSink:
//   - if !rInvalid, then s.r holds the rolling checksum for the first
//     `z.seqMatches` blocks in `buf`.
//
// and this is true when the function returns without error.
func (s *SeedSink) scan(offset int64) error {
	x := s.bufOffset // just a local copy
	xLimit := s.bufLen - s.context

	for x < xLimit {
		blocksMatched := 0

		if s.rInvalid {
			s.r[0] = CalcRsumBlock(s.buf[x : x+int(s.z.blockSize)])
			if s.z.seqMatches > 1 {
				s.r[1] = CalcRsumBlock(s.buf[x+int(s.z.blockSize) : x+2*int(s.z.blockSize)])
			}
			s.rInvalid = false
		}

		// Advance one byte at a time through the input
		entries, found := s.z.hashLookup(s.r)
		if found {
			thismatch, err := s.z.checkChecksumsOnHashChain(entries, s.r, s.buf[x:])
			if err != nil {
				s.bufOffset = x
				return err
			}
			if thismatch > 0 {
				blocksMatched = s.z.seqMatches
				s.BlocksMatched += thismatch
			}
		}

		if blocksMatched == 0 {
			// Advance window by 1 byte - update rolling checksum
			updateRsum(&s.r[0], s.buf[x], s.buf[x+int(s.z.blockSize)], uint(s.z.blockShift))
			if s.z.seqMatches > 1 {
				updateRsum(&s.r[1], s.buf[x+int(s.z.blockSize)], s.buf[x+2*int(s.z.blockSize)], uint(s.z.blockShift))
			}
			x++
		} else {
			// If we got a hit, skip past those blocks. It is highly unlikely that there
			// is a hit at x+1 or any other offset before the next unmatched block,
			// because all of the target blocks are multiples of the blocksize apart.
			x += blocksMatched << s.z.blockShift
			s.rInvalid = true
		}
	}

	s.bufOffset = x
	return nil
}

// Discards processed data from the front of s.buf[]
func (s *SeedSink) shiftBuffer() {
	copy(s.buf, s.buf[s.bufOffset:s.bufLen])
	s.bufLen -= s.bufOffset
	s.bufOffset = 0
}

func (s *SeedSink) ReadFrom(r io.Reader) (N int64, err error) {
	for {
		s.shiftBuffer()
		var n int
		n, err = r.Read(s.buf[s.bufLen:])
		N += int64(n)
		s.bufLen += n
		if err != nil {
			if err == io.EOF {
				break
			}
			return
		}

		// If that filled the buffer, process some data.
		if s.bufLen == len(s.buf) {
			err = s.scan(N - int64(s.bufLen))
			if err != nil {
				return
			}
			if s.bufOffset == 0 {
				panic("current position did not advance")
			}
		}
		if s.progressCallback != nil {
			s.progressCallback(N)
		}
	}
	s.shiftBuffer()
	// Pad with zeros at EOF.
	if s.bufLen < len(s.buf) {
		copy(s.buf[s.bufLen:], make([]byte, len(s.buf)-s.bufLen))
	}
	err = s.scan(N - int64(s.bufLen))
	if s.progressCallback != nil {
		s.progressCallback(N)
	}
	return
}

// checkChecksumsOnHashChain checks data against all blocks for a specific
// hashed rsum value.
// Arguments:
// entries: the list of possible target blocks with matching rsums;
//
//	this is usually the hash chain for the hashed value of block(s) in data[].
//
// data: should be z.blocksize*z.seqMatches bytes of candidate data.
func (z *RcksumState) checkChecksumsOnHashChain(entries []BlockID, r [2]RSum, data []byte) (int, error) {
	gotBlocks := 0

	// MD4sums for blocks in data[], lazily populated as needed.
	var md4sum [2][ChecksumSize]byte
	donemd4 := 0  // How many entries in `md4sum` are populated.

	// Iterate through all matching blocks in this hash bucket.
	// Note that we copied the hash before we start this iteration, so we can
	// remove blocks from rs.rsumHash during this iteration without problems.
	for _, id := range entries {
		z.stats.HashHit++

		if z.rsums[id].A != (r[0].A&z.rsumAMask) || z.rsums[id].B != r[0].B {
			z.stats.HashFalsePositive++
			continue
		}
		if z.seqMatches > 1 && (z.rsums[id+BlockID(1)].A != (r[1].A&z.rsumAMask) || z.rsums[id+BlockID(1)].B != r[1].B) {
			z.stats.HashFalsePositive++
			continue
		}

		z.stats.WeakHit++

		// Calculate strong checksums and see if we have `seqMatches` consecutive
		// matching blocks.
		// MD4sums for blocks in data[] are stored in md4sum[], lazily populated as needed.
		matching := 0
		for checkmd4 := 0; checkmd4 < z.seqMatches; checkmd4++ {
			if checkmd4 >= donemd4 {
				offset := checkmd4 * int(z.blockSize)
				if offset+int(z.blockSize) > len(data) {
					break
				}
				md4sum[checkmd4] = CalcChecksum(data[offset : offset+int(z.blockSize)])
				donemd4++
				z.stats.Checksummed++
			}

			if bytes.Equal(md4sum[checkmd4][:z.checksumBytes], z.md4Checksums[id+BlockID(checkmd4)][:z.checksumBytes]) {
				matching += 1
			} else {
				break
			}
		}

		if matching < z.seqMatches {
			continue
		}

		z.stats.StrongHit += matching

		// Find the next block which we already have.
		nextKnown := z.knownBlocks.nextContainedAfter(id)
		if nextKnown == -1 {
			nextKnown = z.blocks
		}

		numWriteBlocks := matching
		if nextKnown < id+BlockID(matching) {
			numWriteBlocks = int(nextKnown - id)
		}

		// Write the matched blocks
		err := z.writeBlocks(data[:numWriteBlocks*int(z.blockSize)], id, id+BlockID(numWriteBlocks-1))
		if err != nil {
			return gotBlocks, err
		}
		gotBlocks += numWriteBlocks
	}

	if gotBlocks > 0 {
		z.removeFromHash(r)
	}
	return gotBlocks, nil
}

// writeBlocks writes a range of blocks to the output file
func (z *RcksumState) writeBlocks(data []byte, bfrom, bto BlockID) error {
	if z.fd == nil {
		return fmt.Errorf("no file descriptor in RcksumState")
	}

	if int(bto+1-bfrom)<<uint(z.blockShift) != len(data) {
		panic(fmt.Sprintf("missized data block; len=%d written for %d-%d", len(data), bfrom, bto))
	}
	offset := int64(bfrom) << uint(z.blockShift)
	_, err := z.fd.WriteAt(data, offset)
	if err != nil {
		return err
	}

	// Mark blocks as obtained.
	for id := bfrom; id <= bto; id++ {
		z.knownBlocks.addToRanges(id)
	}

	return nil
}

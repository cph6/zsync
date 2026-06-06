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
	"math"
	"os"
)

// SubmitBlocks tests and accepts data blocks matching the target checksums
func (z *RcksumState) SubmitBlocks(data []byte, bfrom, bto BlockID) error {
	z.mu.Lock()
	defer z.mu.Unlock()
	if z.rsumHash == nil {
		if err := z.buildHash(); err != nil {
			return err
		}
	}

	var x BlockID
	// Check each block to see what the highest matching index is.
	for x = bfrom; x <= bto; x++ {
		offset := int64((x - bfrom) << uint(z.blockShift))
		blockData := data[offset : offset+z.blockSize]
		md4sum := CalcChecksum(blockData)

		if !bytes.Equal(md4sum[:z.checksumBytes], z.blockHashes[x].md4[:z.checksumBytes]) {
			break
		}
	}

	_, err := z.writeBlocks(data, bfrom, x-1, 0 /* no next */)
	return err
}

// submitSourceData searches data for target blocks and, if found, writes them to the target file.
// `data` is the data to be checked for matching blocks.
// `offset` is the offset in the source data stream of this block.
// This function expects certain state to be conserved in the RcksumState:
//   - if `offset` is non-zero, it assumes that z.r holds the rolling checksum for
//     the first `seqMatches` blocks in `data[z.skip:]`.
//
// and this is true when the function returns without error.
// TODO: this inherently single-threaded design carried over from the C version
// and should be replaced.
func (z *RcksumState) submitSourceData(data []byte, offset int64) (int, error) {
	x := 0
	gotBlocks := 0
	xLimit := len(data) - int(z.context)

	if offset != 0 {
		x = z.skip
	}
	z.skip = 0

	if x != 0 || offset == 0 {
		z.r[0] = CalcRsumBlock(data[x : x+int(z.blockSize)])
		if z.seqMatches > 1 {
			z.r[1] = CalcRsumBlock(data[x+int(z.blockSize) : x+2*int(z.blockSize)])
		}
	}

	for x < xLimit {
		blocksMatched := 0

		// Advance one byte at a time through the input
		for blocksMatched == 0 && x < xLimit {
			blockID, found := z.hashLookup(z.r)
			if found {
				thismatch, err := z.checkChecksumsOnHashChain(blockID, data[x:])
				if err != nil {
					return gotBlocks, err
				}
				if thismatch > 0 {
					blocksMatched = z.seqMatches
					gotBlocks += thismatch
				}
			}

			if blocksMatched == 0 {
				// Advance window by 1 byte - update rolling checksum
				if x+int(z.blockSize)*z.seqMatches < len(data) {
					updateRsum(&z.r[0], data[x], data[x+int(z.blockSize)], uint(z.blockShift))
					if z.seqMatches > 1 {
						updateRsum(&z.r[1], data[x+int(z.blockSize)], data[x+2*int(z.blockSize)], uint(z.blockShift))
					}
				}
				x++
			}
		}

		// If we got a hit, skip past those blocks. It is highly unlikely that there
		// is a hit at x+1 or any other offset before the next unmatched block,
		// because all of the target blocks are multiples of the blocksize apart.
		if blocksMatched > 0 {
			x += int(z.blockSize) * blocksMatched

			if x <= xLimit {
				// Recalculate rolling checksums for the next blocks
				if z.seqMatches > 1 && blocksMatched == 1 {
					z.r[0] = z.r[1]
				} else if x+int(z.blockSize) <= len(data) {
					z.r[0] = CalcRsumBlock(data[x : x+int(z.blockSize)])
				}
				if z.seqMatches > 1 && x+2*int(z.blockSize) <= len(data) {
					z.r[1] = CalcRsumBlock(data[x+int(z.blockSize) : x+2*int(z.blockSize)])
				}
			}
		}
	}

	z.skip = x - xLimit

	return gotBlocks, nil
}

// SubmitSourceFile reads a file and identifies matching blocks
func (z *RcksumState) SubmitSourceFile(f io.Reader, showProgress bool) (int, error) {
	gotBlocks := 0
	gotBlocksAtLastProgress := 0
	bufSize := int(z.blockSize) * 16
	buf := make([]byte, bufSize)
	var offset int64 // Offset in the source file that the start of buf[] corresponds to.
	lastProgress := offset
	firstBlock := true

	// Build hash tables if needed
	z.mu.Lock()
	defer z.mu.Unlock()
	if z.rsumHash == nil {
		if err := z.buildHash(); err != nil {
			return 0, err
		}
	}

	for {
		if showProgress && offset >= lastProgress+(1<<20) {
			useFraction := float64(gotBlocks-gotBlocksAtLastProgress) * float64(z.blockSize) / float64(offset-lastProgress)
			progressDecile := min(9, int(math.Ceil(useFraction*10)))
			fmt.Fprintf(os.Stderr, "%d", progressDecile)
			lastProgress = offset
			gotBlocksAtLastProgress = gotBlocks
		}

		var len int // The number of bytes of data in buf[]
		if firstBlock {
			// First read
			n, err := f.Read(buf)
			if err != nil && err != io.EOF {
				return gotBlocks, err
			}
			firstBlock = false
			len = n
			offset = 0
		} else {
			// Move the last `context` bytes to the start of the
			// buffer, then refill the rest.
			copy(buf, buf[bufSize-int(z.context):bufSize])
			len = int(z.context)
			offset += int64(bufSize) - z.context

			n, err := f.Read(buf[z.context:])
			if err != nil && err != io.EOF {
				return gotBlocks, err
			}
			len += n
		}

		// Pad with zeros at EOF.
		if len < bufSize {
			copy(buf[len:], make([]byte, bufSize-len))
			len += int(z.context)
			if len > bufSize {
				len = bufSize
			}
		}

		// Process the buffer
		result, err := z.submitSourceData(buf[:len], offset)
		if err != nil {
			return gotBlocks, err
		}
		gotBlocks += result

		if len < bufSize {
			break
		}
	}

	return gotBlocks, nil
}

// checkChecksumsOnHashChain checks data against all blocks for a specific
// hashed rsum value.
// Arguments:
// id: the first possible target block in the hash chain to consider;
//
//	this is usually the start of the hash chain for the hashed value of block(s) in data[].
//
// data: should be z.blocksize*z.seqMatches bytes of candidate data.
func (z *RcksumState) checkChecksumsOnHashChain(id BlockID, data []byte) (int, error) {
	r := z.r[0]
	gotBlocks := 0

	md4sum := [][ChecksumSize]byte{}

	// Invariants:
	// - we are traversing the hash chain starting at block `next`.
	// - the MD4sums of the next n blocks of data in `data` are calculated and
	//   stored in `md4sum`.
	next := id
	for next != noBlock {
		id = next // entry being considered in this run of the loop.
		// Advance the pointer now, as blockHashes[e].next will be wiped if the block
		// is matched.
		next = z.blockHashes[id].next

		// Check weak checksum first.
		z.stats.HashHit++
		if z.blockHashes[id].rsum.A != (r.A&z.rsumAMask) || z.blockHashes[id].rsum.B != r.B {
			continue
		}
		if z.seqMatches > 1 && (z.blockHashes[id+BlockID(1)].rsum.A != (z.r[1].A&z.rsumAMask) || z.blockHashes[id+BlockID(1)].rsum.B != z.r[1].B) {
			continue
		}
		z.stats.WeakHit++

		// Calculate strong checksums and see if we have `seqMatches` consecutive
		// matching blocks.
		// MD4sums for blocks in data[] are stored in md4sum[], lazily populated as needed.
		matching := 0
		for checkmd4 := 0; checkmd4 < z.seqMatches; checkmd4++ {
			if checkmd4 >= len(md4sum) {
				offset := checkmd4 * int(z.blockSize)
				if offset+int(z.blockSize) > len(data) {
					break
				}
				md4sum = append(md4sum, CalcChecksum(data[offset:offset+int(z.blockSize)]))
				z.stats.Checksummed++
			}

			if bytes.Equal(md4sum[checkmd4][:z.checksumBytes], z.blockHashes[id+BlockID(checkmd4)].md4[:z.checksumBytes]) {
				matching += 1
			} else {
				break
			}
		}

		if matching < z.seqMatches {
			continue
		}

		// Find the next block which we already have.
		z.stats.StrongHit += matching
		nextKnown := z.knownBlocks.nextContainedAfter(id)
		if nextKnown == -1 {
			nextKnown = z.blocks
		}

		numWriteBlocks := matching
		if nextKnown < id+BlockID(matching) {
			numWriteBlocks = int(nextKnown - id)
		}

		// Write the matched blocks
		var err error
		next, err = z.writeBlocks(data[:numWriteBlocks*int(z.blockSize)], id, id+BlockID(numWriteBlocks-1), next)
		if err != nil {
			return gotBlocks, err
		}
		gotBlocks += numWriteBlocks
	}

	return gotBlocks, nil
}

// writeBlocks writes a range of blocks to the output file
func (z *RcksumState) writeBlocks(data []byte, bfrom, bto, next BlockID) (BlockID, error) {
	if z.fd == nil {
		return next, fmt.Errorf("no file descriptor in RcksumState")
	}

	if int(bto+1-bfrom)<<uint(z.blockShift) != len(data) {
		panic(fmt.Sprintf("missized data block; len=%d written for %d-%d", len(data), bfrom, bto))
	}
	offset := int64(bfrom) << uint(z.blockShift)
	_, err := z.fd.WriteAt(data, offset)
	if err != nil {
		return next, err
	}

	// Mark blocks as obtained.
	for id := bfrom; id <= bto; id++ {
		// Removing a block from the hash wipes its `next` field, so we have to tell
		// the caller what is the next block that it should consider.
		if id == next {
			next = z.blockHashes[id].next
		}

		z.knownBlocks.addToRanges(id)
		if z.seqMatches == 2 && (id != bto || z.knownBlocks.contains(bto+1)) {
			z.removeBlockFromHash(id)
		}
	}
	// Removing blocks from the hash when we have seqMatches == 2 (the only
	// supported setting other than 1) is tricky; we want to remove only when both
	// a block and its trailing partner are known. So here we look at block
	// `bfrom-1` and remove its hash entry if we delayed removing it earlier
	// because `bfrom`` was not then known.
	if z.seqMatches == 2 && bfrom > 0 && z.knownBlocks.contains(bfrom-1) {
		z.removeBlockFromHash(bfrom - 1)
	}

	return next, nil
}

// ReadKnownData reads back data that has already been received
func (z *RcksumState) ReadKnownData(buf []byte, offset int64) (int, error) {
	z.mu.Lock()
	defer z.mu.Unlock()
	if z.fd == nil {
		return 0, nil
	}
	fd := z.fd

	return fd.ReadAt(buf, offset)
}

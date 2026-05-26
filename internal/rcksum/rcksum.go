// Package rcksum is an implementation of the rsync
// rolling checksum algorithm, used to find desired blocks
// of data out of a data stream.
//
// Typical usage:
// - create object holding state (New),
// - specify the target blocks (AddTargetBlock) by their checksums,
// - pass it a temporary file in which to write target data (SetTargetFile).
// - feed it data that might contain target blocks (SubmitSourceFile).
// - ask what blocks are still missing (NeededBlockRanges).
// - feed it the missing target blocks (SubmitBlocks).
// - check that you are done (BlocksTodo)
//
// Methods are available for retrieving stats during
// reconstruction, and for calculating the block
// checksums.
package rcksum

/*
 * SPDX-FileCopyrightText: 2004,2005,2007,2009,2025,2026 Colin Phipps <cph@moria.org.uk>
 *
 * SPDX-License-Identifier: Artistic-2.0
 */

// AI: copilot / claude code (if I remember correctly) conversion of zsync's rcksum.c.

import (
	"fmt"
	"os"

	"golang.org/x/crypto/md4"
)

// CalcRsumBlock calculates the rsum for a single block of data
func CalcRsumBlock(data []byte) RSum {
	var a, b uint16
	for _, c := range data {
		a += uint16(c)
		b += a
	}
	return RSum{A: a, B: b}
}

// CalcChecksum returns the MD4 checksum of the given data block
func CalcChecksum(data []byte) [ChecksumSize]byte {
	h := md4.New()
	h.Write(data)
	var result [ChecksumSize]byte
	copy(result[:], h.Sum(nil))
	return result
}

// updateRsum updates the rolling checksum by removing one byte and adding another
func updateRsum(r *RSum, oldC, newC byte, blockShift uint) {
	r.A += uint16(newC) - uint16(oldC)
	r.B += r.A - (uint16(oldC) << blockShift)
}

// New creates and returns an RcksumState with the given properties
func New(nblocks BlockID, blockSize int64, rsumBytes int, checksumBytes uint, requireConsecutiveMatches int) (*RcksumState, error) {
	// Validate block size is a power of two
	if blockSize&(blockSize-1) != 0 {
		return nil, fmt.Errorf("block size must be a power of two, got %d", blockSize)
	}

	z := &RcksumState{
		blocks:        nblocks,
		blockSize:     blockSize,
		checksumBytes: checksumBytes,
		seqMatches:    requireConsecutiveMatches,
		context:       blockSize * int64(requireConsecutiveMatches),
	}

	// Calculate rsumAMask based on rsum bytes
	switch {
	case rsumBytes < 3:
		z.rsumAMask = 0
	case rsumBytes == 3:
		z.rsumAMask = 0xff
	default:
		z.rsumAMask = 0xffff
	}

	z.rsumBits = uint16(rsumBytes * 8)

	// Calculate blockshift (log2(blocksize))
	for i := 0; i < 32; i++ {
		if uint64(blockSize) == 1<<uint(i) {
			z.blockShift = i
			break
		}
	}

	// Allocate hash entries.
	z.blockHashes = make([]hashEntry, nblocks)

	// Initialize ranges and other state
	z.knownBlocks.ranges = make([]blockIDPair, 0)
	z.knownBlocks.gotBlocks = 0

	return z, nil
}

// SetTargetFile adds a file handle to the rcksum state to
// be used for reconstructing the target file.
// The file should be writeable and will be overwritten.
// rcksum expects to be the sole writer to the file for the duration.
func (z *RcksumState) SetTargetFile(fd *os.File) {
	z.fd = fd
}

// AddTargetBlock sets the stored hash values for the given blockid
func (z *RcksumState) AddTargetBlock(b BlockID, r RSum, checksum [ChecksumSize]byte) {
	if b < z.blocks {
		z.mu.Lock()
		defer z.mu.Unlock()

		// Get hash entry for this block
		e := &z.blockHashes[b]

		// Store checksums
		e.md4 = checksum
		e.rsum.A = r.A & z.rsumAMask
		e.rsum.B = r.B

		// Invalidate existing hash tables since we added new data
		z.rsumHash = nil
		z.bitHash = nil
	}
}

// BlocksTodo returns the number of blocks still needed
func (z *RcksumState) BlocksTodo() int64 {
	z.mu.Lock()
	defer z.mu.Unlock()
	return int64(z.blocks - BlockID(z.knownBlocks.gotBlocks))
}

// NeededBlockRanges returns the ranges of blocks that are still needed
func (z *RcksumState) NeededBlockRanges(from, to BlockID) []blockIDPair {
	return z.knownBlocks.missingBlocksBetween(from, to)
}

// Stats returns stats on the rolling checksum process.
func (z *RcksumState) Stats() Stats {
	return z.stats
}

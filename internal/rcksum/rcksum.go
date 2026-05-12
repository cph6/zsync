package rcksum

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

// UpdateRsum updates the rolling checksum by removing one byte and adding another
func UpdateRsum(r *RSum, oldC, newC byte, blockShift uint) {
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

	// Create temporary file
	// TODO: temp path needs to be in the dir with the output file.
	tmpFile, err := os.CreateTemp(".", "rcksum-*")
	if err != nil {
		return nil, fmt.Errorf("failed to create temporary file: %w", err)
	}
	z.filename = tmpFile.Name()
	z.fd = tmpFile

	// Allocate hash entries.
	z.blockHashes = make([]hashEntry, nblocks)

	// Initialize ranges and other state
	z.knownBlocks.ranges = make([]blockIDPair, 0)
	z.knownBlocks.gotBlocks = 0

	return z, nil
}

// Close cleans up resources associated with the RcksumState
func (z *RcksumState) Close() error {
	var err error

	// Close the file descriptor
	if z.fd != nil {
		err = z.fd.Close()
		z.fd = nil
	}

	// Delete the temporary file
	if z.filename != "" {
		_ = os.Remove(z.filename)
		z.filename = ""
	}

	return err
}

// Filename returns the temporary filename and transfers ownership to the caller
// After this call, the RcksumState will not manage the file
func (z *RcksumState) Filename() string {
	filename := z.filename
	z.filename = ""
	return filename
}

// File returns the file descriptor for the temporary file
// After this call, the RcksumState will not manage the file
func (z *RcksumState) File() *os.File {
	fd := z.fd
	z.fd = nil
	return fd
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
	return int64(z.blocks - BlockID(z.knownBlocks.gotBlocks))
}

// NeededBlockRanges returns the ranges of blocks that are still needed
func (z *RcksumState) NeededBlockRanges(from, to BlockID) []blockIDPair {
	return z.knownBlocks.missingBlocksBetween(from, to)
}

// Filehandle returns the file descriptor for the temporary file
func (z *RcksumState) Filehandle() *os.File {
	return z.fd
}

// Returns stats on the rolling checksum process.
func (z *RcksumState) Stats() Stats {
	return z.stats
}

package rcksum

/*
 * SPDX-FileCopyrightText: 2004,2005,2007,2009,2025,2026 Colin Phipps <cph@moria.org.uk>
 *
 * SPDX-License-Identifier: Artistic-2.0
 */

// AI: copilot / claude code (if I remember correctly) conversion of zsync's rcksum.h and perhaps other files in the same module.

import (
	"os"
	"sync"

	"golang.org/x/crypto/md4"
)

const (
	// ChecksumSize is the size of MD4 checksums in bytes
	ChecksumSize = md4.Size

	// BithashBits is the number of bits per byte for the bithash table
	BithashBits = 4
)

// BlockID represents an identifier for a block in the target file
type BlockID int32

// RSum represents a rolling checksum (Adler-style)
type RSum struct {
	A uint16 // Sum of bytes
	B uint16 // Weighted sum
}

// MD4Checksum represents an MD4 checksum
type MD4Checksum [ChecksumSize]byte

// Stats tracks statistics about the matching process
type Stats struct {
	BithashHit        int64 // Number of hash table hits
	HashHit           int64 // Number of hash table hits
	HashFalsePositive int64 // Number of hash table entries traversed with mismatching rsums
	WeakHit           int   // Number of weak checksum hits
	StrongHit         int   // Number of strong checksum hits
	Checksummed       int   // Number of checksums calculated
}

// RcksumState contains the set of checksums of the blocks of a target file
// and is used to apply the rsync algorithm to detect data in common with a local file.
type RcksumState struct {
	// Block configuration
	blocks        BlockID // Number of blocks in the target file
	blockSize     int64   // Number of bytes per block
	blockShift    int     // log2(blocksize)
	rsumAMask     uint16  // Mask to apply to rsum values before lookup
	rsumBits      uint16  // Number of bits of rsum data
	checksumBytes uint    // Number of bytes of the MD4 checksum available
	seqMatches    int     // Required consecutive matches

	// MD4 checksums for each block
	md4Checksums []MD4Checksum
	// Rolling checksums for each block (used for hash table)
	rsums []RSum

	// Hash tables for rsync algorithm
	rsumHash map[uint32][]BlockID
	// Bithash for fast negative lookups.
	bitHash     []byte
	bitHashMask uint32

	// Known data ranges
	knownBlocks blockRanges

	// Statistics
	stats Stats

	// Temporary file for output
	fd *os.File

	// Synchronization for thread safety
	mu sync.Mutex
}

// MatchedBlock represents a block that matched during the algorithm
type MatchedBlock struct {
	BlockID  BlockID
	Checksum [ChecksumSize]byte
	WeakSum  RSum
}

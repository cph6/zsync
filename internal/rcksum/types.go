// Package rcksum implements the rsync rolling checksum algorithm for determining
// which parts of a file you have and which you need.
// Copyright (C) 2004,2005,2009 Colin Phipps <cph@moria.org.uk>
// Converted to Go 2026
//
// This program is free software; you can redistribute it and/or modify
// it under the terms of the Artistic License v2 (see the accompanying
// file COPYING for the full license terms), or, at your option, any later
// version of the same license.
package rcksum

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
	BithashBits = 3

	noBlock = BlockID(-1)
)

// BlockID represents an identifier for a block in the target file
type BlockID int32

// RSum represents a rolling checksum (Adler-style)
type RSum struct {
	A uint16 // Sum of bytes
	B uint16 // Weighted sum
}

type hashEntry struct {
	next BlockID // -1 means no next element in this hash chain.
	rsum RSum
	md4  [16]byte
}

// Stats tracks statistics about the matching process
type Stats struct {
	HashHit     int64 // Number of hash table hits
	WeakHit     int   // Number of weak checksum hits
	StrongHit   int   // Number of strong checksum hits
	Checksummed int   // Number of checksums calculated
}

// RcksumState contains the set of checksums of the blocks of a target file
// and is used to apply the rsync algorithm to detect data in common with a local file.
type RcksumState struct {
	// Current rolling checksums
	r [2]RSum

	// Block configuration
	blocks        BlockID // Number of blocks in the target file
	blockSize     int64   // Number of bytes per block
	blockShift    int     // log2(blocksize)
	rsumAMask     uint16  // Mask to apply to rsum values before lookup
	rsumBits      uint16  // Number of bits of rsum data
	hashFuncShift uint    // Config for the hash function
	checksumBytes uint    // Number of bytes of the MD4 checksum available
	seqMatches    int     // Required consecutive matches
	context       int64   // blockSize * seqMatches

	// Processing state
	skip int // Skip forward on next submit_source_data

	// Hints for matching
	nextMatch BlockID // Hint for next block to match

	// Hash tables for rsync algorithm
	blockHashes []hashEntry
	rsumHash    map[uint32]BlockID

	// Bithash for fast negative lookups.
	bitHash     []byte
	bitHashMask uint32

	// Known data ranges
	knownBlocks blockRanges

	// Statistics
	stats Stats

	// Temporary file for output
	filename string
	fd       *os.File

	// Synchronization for thread safety
	mu sync.Mutex
}

// MatchedBlock represents a block that matched during the algorithm
type MatchedBlock struct {
	BlockID  BlockID
	Checksum [ChecksumSize]byte
	WeakSum  RSum
}

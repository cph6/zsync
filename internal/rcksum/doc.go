// Package documentation and API reference for rcksum

/*
Package rcksum implements the rsync rolling checksum algorithm for determining
which parts of a file you have and which you need.

This is a complete Go port of the C implementation from zsync, using the same
core algorithms and data structures.

# Core Types

RcksumState - The main state object that holds:
  - Block checksums (both weak rolling checksums and strong MD4 hashes)
  - Hash tables for fast block lookup
  - Tracking of known blocks
  - Temporary file for accumulating received data

# RSum - A rolling Adler-style checksum.

# Main Operations

Creating a new state:

	z, err := New(100, 4096, 2, 16, 1)

Adding target blocks:

	r := CalcRsumBlock(blockData)
	checksum := CalcChecksum(blockData)
	z.AddTargetBlock(blockID, r, checksum)

Building hash tables for fast lookup:

	err := z.BuildHash()

Submitting source data for matching:

	matched := z.SubmitSourceData(data, offset)

# How It Works

The library implements the rsync algorithm:

 1. Target blocks are registered with their weak checksums (rolling Adler-style)
    and strong checksums (MD4)
 2. Source data is scanned byte-by-byte
 3. For each block-sized window, the rolling checksum is calculated
 4. The hash tables are used to find potential matches
 5. When found, strong checksums are verified to confirm matches
 6. Matched blocks are written to a temporary file
 7. The algorithm maintains ranges of blocks already received

# Thread Safety

The RcksumState uses internal mutexes for thread-safe access to its data structures.
All public methods are protected by locks.

# Rolling Checksum

The weak checksum is calculated as:

	a = sum of all bytes
	b = sum of (i * byte[i]) for all bytes

This allows for efficient rolling computation:

	When removing byte C from the left and adding byte N to the right:
	new_a = old_a + N - C
	new_b = old_b + new_a - (C << blockshift)

# Performance

The implementation includes:
- Bitmask (bithash) for fast negative lookups
- Hash chains for handling collisions
- Lazy hash table construction
- Support for sequential block matching
*/
package rcksum

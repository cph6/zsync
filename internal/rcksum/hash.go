package rcksum

/*
 * SPDX-FileCopyrightText: 2004,2005,2007,2009,2025,2026 Colin Phipps <cph@moria.org.uk>
 *
 * SPDX-License-Identifier: Artistic-2.0
 */

// AI: copilot / claude code (if I remember correctly) conversion of zsync's hash.c.

import (
	"math/bits"
	"slices"
)

// calcRhashFromRSums calculates hash from two RSums
// rs[1] is required iff z.seqMatches == 2; otherwise it is unused.
func (z *RcksumState) calcRhashFromRSums(rs [2]RSum) uint32 {
	hash := uint32(rs[0].B)

	if z.seqMatches > 1 {
		hash ^= uint32(rs[1].B) << 16
	} else {
		hash ^= uint32(rs[0].A&z.rsumAMask) << 16
	}

	return hash
}

// BuildHash builds hash tables to quickly lookup blocks based on rsum value
func (z *RcksumState) buildHash() error {
	// Allocate hash table
	z.rsumHash = make(map[uint32][]BlockID)

	// Allocate bithash with aim of 1/(1<<BITHASHBITS) load factor
	bitHashBits := log2(uint32(z.blocks)) + BithashBits
	z.bitHashMask = (1 << uint(bitHashBits)) - 1
	z.bitHash = make([]byte, (z.bitHashMask+1+7)>>3)

	for id := BlockID(0); id < z.blocks+BlockID(1-z.seqMatches); id++ {
		// For each block, create a hash entry and add it to the appropriate hash bucket
		var rs [2]RSum
		rs[0] = z.rsums[id]
		if z.seqMatches > 1 {
			rs[1] = z.rsums[id+1]
		}

		h := z.calcRhashFromRSums(rs)

		// Append to the slice for this hash value
		z.rsumHash[h] = append(z.rsumHash[h], id)

		// Set relevant bit in bithash.
		bitIdx := (h & z.bitHashMask) >> 3
		bitPos := h & 7
		if int(bitIdx) < len(z.bitHash) {
			z.bitHash[bitIdx] |= 1 << bitPos
		}
	}

	return nil
}

// hashLookup checks whether the given pair of rolling checksums for
// consecutive blocks are in the hash table of known block pairs for the target
// file.
func (z *RcksumState) hashLookup(rs [2]RSum) ([]BlockID, bool) {
	h := z.calcRhashFromRSums(rs)

	// Check bithash for fast negative lookups
	bitIdx := (h & z.bitHashMask) >> 3
	bitPos := h & 7

	if z.bitHash != nil && int(bitIdx) < len(z.bitHash) {
		if (z.bitHash[bitIdx] & (1 << bitPos)) != 0 {
			z.stats.BithashHit++
			entries, found := z.rsumHash[h]
			return entries, found
		}
	}

	return nil, false
}

// removeFromHash removes a block (or blocks) from the hash table
// once its data is obtained.
// It might seem like this is unnecessary — we have the range tracking
// that tracks what we already know in the target file and prevents us from
// rewriting the data if we do find an identical block in the input. But this
// is actually an important optimisation, because it is common to find highly
// repeated blocks in files (particularly all-0 blocks, but blocks with
// repeating patterns are common also). These are an O(n^2) case for the hash
// lookups - every null block in the input scans the hash chain for every null
// block in the output (for example). Whereas if we remove all the null block
// entries from the hash after writing out all the null blocks, then we only
// have to scan that long hash chain once, giving us a typical O(n) for this
// case.
// For that reason also, the arguments are a single RSum but a range of blocks;
// the RSum should correspond to at least one block within that range.
// Args:
// - rs: the RSum of the block / every block that was removed.
func (z *RcksumState) removeFromHash(rs [2]RSum) {
	h := z.calcRhashFromRSums(rs)

	if entries, found := z.rsumHash[h]; found {
		prunedChain := slices.DeleteFunc(entries, func(id BlockID) bool {
			return z.knownBlocks.contains(id)
		})
		if len(prunedChain) > 0 {
			z.rsumHash[h] = prunedChain
		} else {
			delete(z.rsumHash, h)
		}
	}
}

// log2 returns the base-2 logarithm of x
func log2(x uint32) int {
	if x == 0 {
		return 0
	}
	return bits.Len32(x) - 1
}

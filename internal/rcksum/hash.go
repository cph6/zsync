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
func (z *RcksumState) buildHash() {
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

// removeFromHash removes duplicate blocks from the hash table
// once its data is obtained.
// If there are duplicate blocks in the output and different duplicate blocks
// in the input with colliding hash keys, then there is an O(n^2) case where we
// have to iterate through the duplicate output blocks to reject them for each
// colliding duplicate input block. The case that I have seen is FreeBSD ISO
// images with repeating patterns that have RSum{0,0} (e.g. many blocks with
// 0x00fc repeated and 0x2020 repeated).
// The worst case is O(n^2), but if we remove repeated blocks from the hash
// table after they have been written out then we make the best case
// performance O(n) — we hope that we find such a block early and so can write
// them all and remove them from the chain early, avoiding repeated traversals.
// But removing found blocks from the hash table entirely causes a different
// O(n^2) case. When we first find, say, a repeated 0x00fc block in the input,
// we write it to the relevant output locations and jump past it, but we then
// have to do a rolling checksum of all the later 0x00fc blocks and compare
// them to all of the 0x2020 blocks in the hash table (until we find,
// hopefully, an 0x2020 block).
// So the best approach is to remove duplicate blocks from the hash chain
// *leaving just one representative entry behind*. That way we can match a
// later recurring block against the same checksums as the earlier block, write
// nothing out (because we already saw that data), but still skip over it like
// any matched block.
// Note that, as with skipping forward past matched blocks in general, this
// does causes some misses of local data that could be used for the target
// file. On a FreeBSD ISO, I see +0.2% data transferred, -80% CPU used, -20s
// elapsed time, so it is very worthwhile for the cases where it matters.
// TODO: move this explanation out to the technical paper.
// Args:
// - rs: the RSum of the block / every block that was removed.
func (z *RcksumState) removeFromHash(rs [2]RSum) {
	h := z.calcRhashFromRSums(rs)

	if entries, found := z.rsumHash[h]; found {
		seenChecksum := make(map[StrongChecksum]bool, len(entries))
		prunedChain := slices.DeleteFunc(entries, func(id BlockID) bool {
			if z.knownBlocks.contains(id) {
				sc := z.strongChecksums[id]
				_, ok := seenChecksum[sc]
				seenChecksum[sc] = true
				return ok
			}
			return false
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

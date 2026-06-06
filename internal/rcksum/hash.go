package rcksum

/*
 * SPDX-FileCopyrightText: 2004,2005,2007,2009,2025,2026 Colin Phipps <cph@moria.org.uk>
 *
 * SPDX-License-Identifier: Artistic-2.0
 */

// AI: copilot / claude code (if I remember correctly) conversion of zsync's hash.c.

import (
	"math/bits"
)

// calcRhash calculates the hash key for consecutive hashEntrys.
func (z *RcksumState) calcRhash(b BlockID) uint32 {
	var rs [2]RSum
	rs[0] = z.blockHashes[b].rsum
	if z.seqMatches > 1 {
		rs[1] = z.blockHashes[b+1].rsum
	}
	return z.calcRhashFromRSums(rs)
}

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
	z.rsumHash = make(map[uint32]BlockID)

	// Allocate bithash with aim of 1/(1<<BITHASHBITS) load factor
	bitHashBits := log2(uint32(z.blocks)) + BithashBits
	z.bitHashMask = (1 << uint(bitHashBits)) - 1
	z.bitHash = make([]byte, (z.bitHashMask+1+7)>>3)

	// Fill hash tables in reverse order to keep blocks in order.
	for id := z.blocks - BlockID(z.seqMatches); id >= 0; id-- {
		// Calculate hash and prepend to the linked list for that hash value.
		h := z.calcRhash(id)
		next, ok := z.rsumHash[h]
		if !ok {
			next = noBlock
		}
		z.blockHashes[id].next = next
		z.rsumHash[h] = id

		// Set relevant bit in bithash.
		bitIdx := (h & z.bitHashMask) >> 3
		bitPos := h & 7
		if int(bitIdx) < len(z.bitHash) {
			z.bitHash[bitIdx] |= 1 << bitPos
		}
	}

	return nil
}

// removeBlockFromHash removes a block from the hash table
func (z *RcksumState) removeBlockFromHash(id BlockID) {
	// Ignore any request to remove blocks past the end of the file, or within
	// seqMatches-1 of the end as these were never added to the hash (insufficient
	// trailing blocks for consecutive matches).
	if z.rsumHash == nil || id >= BlockID(len(z.blockHashes)-(z.seqMatches-1)) {
		return
	}

	h := z.calcRhash(id)

	// Find and remove from hash chain
	p, ok := z.rsumHash[h]
	if ok && p == id {
		next := z.blockHashes[id].next
		if next != noBlock {
			z.rsumHash[h] = next
		} else {
			delete(z.rsumHash, h)
		}
	} else if ok {
		for ; p != noBlock; p = z.blockHashes[p].next {
			if z.blockHashes[p].next == id {
				z.blockHashes[p].next = z.blockHashes[id].next
				break
			}
		}
	}

	z.blockHashes[id].next = noBlock
}

// hashLookup checks whether the given pair of rolling checksums for
// consecutive blocks are in the hash table of known block pairs for the target
// file.
func (z *RcksumState) hashLookup(rs [2]RSum) (b BlockID, found bool) {
	h := z.calcRhashFromRSums(rs)

	// Check bithash for fast negative lookups
	bitIdx := (h & z.bitHashMask) >> 3
	bitPos := h & 7

	if z.bitHash != nil && int(bitIdx) < len(z.bitHash) {
		if (z.bitHash[bitIdx] & (1 << bitPos)) != 0 {
			z.stats.BithashHit++
			b, found = z.rsumHash[h]
			return
		}
	}

	return 0, false
}

// log2 returns the base-2 logarithm of x
func log2(x uint32) int {
	if x == 0 {
		return 0
	}
	return bits.Len32(x) - 1
}

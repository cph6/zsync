package rcksum

// AI: copilot / claude code (if I remember correctly) conversion of zsync's hash.c.

import (
	"math/bits"
)

// CalcRhash calculates the hash value for a hash entry
func CalcRhash(z *RcksumState, e *HashEntry) uint32 {
	hash := uint32(e.RSum.B)

	if z.seqMatches > 1 {
		hash ^= uint32(e.RSum.A&z.rsumAMask) << z.hashFuncShift
	} else {
		hash ^= uint32(e.RSum.A&z.rsumAMask) << z.hashFuncShift
	}

	return hash
}

// BuildHash builds hash tables to quickly lookup blocks based on rsum value
func (z *RcksumState) buildHash() error {
	// Calculate available bits
	availBits := z.rsumBits
	if z.seqMatches > 1 {
		bits1 := min(z.rsumBits, 16)
		bits2 := availBits - 16
		availBits = minUint16(bits1*2, availBits)
		if bits2 > 0 {
			availBits = minUint16(availBits, bits1+bits2)
		}
	}

	hashBits := availBits

	// Pick a hash size that is a power of two and gives load factor < 1
	for (1<<(hashBits-1)) > uint16(z.blocks) && hashBits > 5 {
		hashBits--
	}

	// Allocate hash table
	z.hashMask = (1 << hashBits) - 1
	z.rsumHash = make([](*HashEntry), z.hashMask+1)

	// Allocate bithash with aim of 1/(1<<BITHASHBITS) load factor
	bitHashBits := int(hashBits) + BithashBits
	if bitHashBits > int(availBits) {
		bitHashBits = int(availBits)
	}
	z.bitHashMask = (1 << uint(bitHashBits)) - 1
	z.bitHash = make([]byte, (z.bitHashMask+1+7)>>3)

	// Set up hash function shift
	if z.seqMatches > 1 && availBits < 24 {
		// Second number has (availBits/2) bits available
		z.hashFuncShift = uint(maxInt(0, int(hashBits)-int(availBits/2)))
	} else {
		// Second number has availBits-16 bits available
		z.hashFuncShift = uint(maxInt(0, int(hashBits)-int(availBits-16)))
	}

	// Fill hash tables in reverse order to keep blocks in order
	for id := z.blocks - 1; id >= 0; id-- {
		e := &z.blockHashes[id]

		// Calculate hash and prepend to linked list
		h := CalcRhash(z, e)
		e.Next = z.rsumHash[h&z.hashMask]
		z.rsumHash[h&z.hashMask] = e

		// Set relevant bit in bithash
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
	if z.rsumHash == nil || id >= BlockID(len(z.blockHashes)) {
		return
	}

	e := &z.blockHashes[id]
	h := CalcRhash(z, e)
	hashIdx := h & z.hashMask

	// Find and remove from hash chain
	if z.rsumHash[hashIdx] == e {
		z.rsumHash[hashIdx] = e.Next
	} else if z.rsumHash[hashIdx] != nil {
		for chain := z.rsumHash[hashIdx]; chain != nil; chain = chain.Next {
			if chain.Next == e {
				chain.Next = e.Next
				break
			}
		}
	}

	e.Next = nil
}

// min returns the minimum of two uint16 values
func min(a, b uint16) uint16 {
	if a < b {
		return a
	}
	return b
}

// minUint16 is a helper to find minimum of two uint16 values
func minUint16(a, b uint16) uint16 {
	if a < b {
		return a
	}
	return b
}

// maxInt returns the maximum of two int values
func maxInt(a, b int) int {
	if a > b {
		return a
	}
	return b
}

// log2 returns the base-2 logarithm of x
func log2(x uint32) int {
	if x == 0 {
		return 0
	}
	return bits.Len32(x) - 1
}

// PopCount returns the number of set bits (population count)
func PopCount(b byte) int {
	return bits.OnesCount8(b)
}

// GetHEBlockID returns the block ID for a hash entry
func GetHEBlockID(z *RcksumState, e *HashEntry) BlockID {
	// Find the index of e in blockHashes
	for id := BlockID(0); id < BlockID(len(z.blockHashes)); id++ {
		if &z.blockHashes[id] == e {
			return id
		}
	}
	return -1
}

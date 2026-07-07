package rcksum

/*
 * SPDX-FileCopyrightText: 2026 Colin Phipps <cph@moria.org.uk>
 *
 * SPDX-License-Identifier: Artistic-2.0
 */

// AI: copilot / GPT mini

import (
	"bytes"
	"crypto"
	"testing"
)

// helper to check presence of a BlockID in a slice
func containsBlock(s []BlockID, id BlockID) bool {
	for _, v := range s {
		if v == id {
			return true
		}
	}
	return false
}

// Test that blocks added to the hash table for seqMatches==1 can be looked up
func TestHashLookupSingle(t *testing.T) {
	const nblocks = 6
	const blockSize = 16

	// use rsumBytes=4 to get full rsumAMask (0xffff)
	z, err := New(nblocks, blockSize, crypto.MD4, 4, MaxChecksumSize, 1)
	if err != nil {
		t.Fatalf("New failed: %v", err)
	}

	// add blocks with distinct contents
	for i := 0; i < nblocks; i++ {
		data := bytes.Repeat([]byte{byte(i + 1)}, blockSize)
		r := CalcRsumBlock(data)
		checksum := CalcChecksum(data, crypto.MD4)
		z.AddTargetBlock(BlockID(i), r, checksum)
	}

	// build hash
	z.Prepare()

	// Pick a block and lookup by its rsum
	want := BlockID(3)
	rs := [2]RSum{z.rsums[want]}

	entries, found := z.hashLookup(rs)
	if !found {
		t.Fatalf("hashLookup did not find block %d", want)
	}
	if !containsBlock(entries, want) {
		t.Fatalf("hashLookup entries do not contain expected block %d: %#v", want, entries)
	}
}

// Test that blocks added to the hash table for seqMatches==2 (pairs) can be looked up
func TestHashLookupSeq2(t *testing.T) {
	const nblocks = 6
	const blockSize = 16

	// rsumBytes=4 so rsumAMask is wide; seqMatches=2 to use two-block hashes
	z, err := New(nblocks, blockSize, crypto.MD4, 4, MaxChecksumSize, 2)
	if err != nil {
		t.Fatalf("New failed: %v", err)
	}

	// add blocks with distinct contents
	for i := 0; i < nblocks; i++ {
		data := bytes.Repeat([]byte{byte(i + 1)}, blockSize)
		r := CalcRsumBlock(data)
		checksum := CalcChecksum(data, crypto.MD4)
		z.AddTargetBlock(BlockID(i), r, checksum)
	}

	z.Prepare()

	// For seqMatches==2 we can only look up starting blocks that have a following block
	want := BlockID(2)
	rs := [2]RSum{z.rsums[want], z.rsums[want+1]}

	entries, found := z.hashLookup(rs)
	if !found {
		t.Fatalf("hashLookup did not find block pair starting at %d", want)
	}
	if !containsBlock(entries, want) {
		t.Fatalf("hashLookup entries do not contain expected block %d: %#v", want, entries)
	}
}

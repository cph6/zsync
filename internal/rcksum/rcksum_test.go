package rcksum

import (
	"bytes"
	"testing"
)

// TestCalcRsumBlock tests the rolling checksum calculation
func TestCalcRsumBlock(t *testing.T) {
	data := []byte("hello world")
	r := CalcRsumBlock(data)

	if r.A != 1116 {
		t.Errorf("Expected A=1116, got %d", r.A)
	}
	if r.B != 6656 {
		t.Errorf("Expected B=6656, got %d", r.B)
	}
}

// TestCalcChecksum tests MD4 checksum calculation
func TestCalcChecksum(t *testing.T) {
	data := []byte("hello world")
	sum := CalcChecksum(data)

	if sum != [ChecksumSize]byte{
		0xaa, 0x01, 0x0f, 0xbc, 0x1d, 0x14, 0xc7, 0x95, 0xd8, 0x6e, 0xf9, 0x8c, 0x95, 0x47, 0x9d, 0x17} {
		t.Errorf("Unexpected checksum: %x", sum)
	}
}

// TestRcksumStateCreation tests creating an RcksumState
func TestRcksumStateCreation(t *testing.T) {
	z, err := New(100, 4096, 2, ChecksumSize, 1)
	if err != nil {
		t.Fatalf("Failed to create RcksumState: %v", err)
	}

	if z.blocks != 100 {
		t.Errorf("Expected 100 blocks, got %d", z.blocks)
	}
	if z.blockSize != 4096 {
		t.Errorf("Expected blockSize 4096, got %d", z.blockSize)
	}
	if z.blockShift != 12 {
		t.Errorf("Expected blockShift 12 (log2(4096)), got %d", z.blockShift)
	}
}

// TestAddTargetBlock tests adding a target block
func TestAddTargetBlock(t *testing.T) {
	z, err := New(10, 4096, 2, ChecksumSize, 1)
	if err != nil {
		t.Fatalf("Failed to create RcksumState: %v", err)
	}

	r := RSum{A: 100, B: 200}
	checksum := CalcChecksum([]byte("test data"))

	z.AddTargetBlock(0, r, checksum)
	z.AddTargetBlock(1, r, checksum)

	// Verify blocks were added
	if z.blockHashes[0].rsum.A != (100 & z.rsumAMask) {
		t.Error("Block 0 rsum not set correctly")
	}
	if z.blockHashes[1].rsum.A != (100 & z.rsumAMask) {
		t.Error("Block 1 rsum not set correctly")
	}
}

// TestUpdateRsum tests the rolling checksum update
func TestUpdateRsum(t *testing.T) {
	r := RSum{A: 100, B: 200}

	updateRsum(&r, 'a', 'b', 12)

	if r.A != 101 || r.B != 61741 {
		t.Error("Unexpected result from updateRsum: A=", r.A, "B=", r.B)
	}
}

// TestHashTableBuilding tests building hash tables
func TestHashTableBuilding(t *testing.T) {
	z, err := New(10, 4096, 2, ChecksumSize, 1)
	if err != nil {
		t.Fatalf("Failed to create RcksumState: %v", err)
	}

	// Add some target blocks
	for i := 0; i < 10; i++ {
		data := bytes.Repeat([]byte{byte(i)}, 4096)
		r := CalcRsumBlock(data)
		checksum := CalcChecksum(data)
		z.AddTargetBlock(BlockID(i), r, checksum)
	}

	// Build hash tables
	err = z.buildHash()
	if err != nil {
		t.Fatalf("Failed to build hash tables: %v", err)
	}
}

// TestBlocksTodo tests the blocks remaining count
func TestBlocksTodo(t *testing.T) {
	z, err := New(100, 4096, 2, ChecksumSize, 1)
	if err != nil {
		t.Fatalf("Failed to create RcksumState: %v", err)
	}

	if z.BlocksTodo() != 100 {
		t.Errorf("Expected 100 remaining blocks, got %d", z.BlocksTodo())
	}

	z.knownBlocks.addToRanges(0)
	if z.BlocksTodo() != 99 {
		t.Errorf("Expected 99 remaining blocks, got %d", z.BlocksTodo())
	}
}

// TestNeededBlockRanges tests getting ranges of needed blocks
func TestNeededBlockRanges(t *testing.T) {
	z, err := New(100, 4096, 2, ChecksumSize, 1)
	if err != nil {
		t.Fatalf("Failed to create RcksumState: %v", err)
	}

	// Mark blocks 10-19 as obtained
	for i := 10; i < 20; i++ {
		z.knownBlocks.addToRanges(BlockID(i))
	}

	ranges := z.knownBlocks.missingBlocksBetween(0, 99)
	if len(ranges) == 0 {
		t.Error("missingBlocksBetween returned empty when there should be needed blocks")
	}

	compareRanges(t, ranges, []blockIDPair{{0, 9}, {20, 99}})
}

// BenchmarkCalcRsumBlock benchmarks rolling checksum calculation
func BenchmarkCalcRsumBlock(b *testing.B) {
	data := bytes.Repeat([]byte("test"), 1024)
	b.ResetTimer()
	for i := 0; i < b.N; i++ {
		CalcRsumBlock(data)
	}
}

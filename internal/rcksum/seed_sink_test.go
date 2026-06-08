package rcksum

import (
	"bytes"
	"os"
	"testing"
)

// TestSeedSink verifies SeedSink finds matching blocks in a stream
func TestSeedSink(t *testing.T) {
	const nblocks = 8
	const blockSize = 16

	z, err := New(nblocks, blockSize, 4, ChecksumSize, 1)
	if err != nil {
		t.Fatalf("New failed: %v", err)
	}

	// create temp file for writing matched blocks
	f, err := os.CreateTemp("", "rcksum-test-*")
	if err != nil {
		t.Fatalf("CreateTemp failed: %v", err)
	}
	defer func() {
		f.Close()
		os.Remove(f.Name())
	}()
	z.SetTargetFile(f)

	// prepare target blocks: blocks 2 and 5 will be expected to match
	var blocks [][]byte
	for i := 0; i < nblocks; i++ {
		data := bytes.Repeat([]byte{byte(i + 1)}, blockSize)
		blocks = append(blocks, data)
		z.AddTargetBlock(BlockID(i), CalcRsumBlock(data), CalcChecksum(data))
	}

	// Build hash tables
	z.Prepare()

	// Construct a stream containing: garbage, block2, garbage, block5, garbage
	stream := make([]byte, 0)
	stream = append(stream, bytes.Repeat([]byte{0xaa}, 10)...) // some leading garbage
	stream = append(stream, blocks[2]...)
	stream = append(stream, bytes.Repeat([]byte{0xbb}, 7)...) // garbage between
	stream = append(stream, blocks[5]...)
	stream = append(stream, bytes.Repeat([]byte{0xcc}, 20)...) // trailing garbage

	// Feed stream into SeedSink
	sink := z.NewSeedSink(nil)
	_, err = sink.ReadFrom(bytes.NewReader(stream))
	if err != nil {
		t.Fatalf("ReadFrom failed: %v", err)
	}

	// Check that blocks 2 and 5 were recorded as known
	if !z.knownBlocks.contains(2) {
		t.Errorf("expected block 2 to be known")
	}
	if !z.knownBlocks.contains(5) {
		t.Errorf("expected block 5 to be known")
	}
	if z.knownBlocks.contains(3) {
		t.Errorf("expected block 3 to be unknown")
	}
	if z.knownBlocks.contains(4) {
		t.Errorf("expected block 5 to be unknown")
	}

	// Verify the file contains the blocks at the correct offsets
	// Read back data
	buf := make([]byte, blockSize*nblocks)
	_, err = f.ReadAt(buf, 0)
	if err != nil {
		// it's OK if the file isn't fully written; we just want to check positions
	}

	// block 2 should be at offset 2*blockSize
	got2 := buf[2*blockSize : 3*blockSize]
	if !bytes.Equal(got2, blocks[2]) {
		t.Errorf("file contents at block 2 do not match")
	}
	got5 := buf[5*blockSize : 6*blockSize]
	if !bytes.Equal(got5, blocks[5]) {
		t.Errorf("file contents at block 5 do not match")
	}
}

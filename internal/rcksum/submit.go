package rcksum

import (
	"bytes"
	"io"
)

// SubmitBlocks tests and accepts data blocks matching the target checksums
func (z *RcksumState) SubmitBlocks(data []byte, bfrom, bto BlockID) int {
	z.mu.Lock()
	defer z.mu.Unlock()
	if z.rsumHash == nil {
		if err := z.buildHash(); err != nil {
			return -1
		}
	}

	// Check each block
	for x := bfrom; x <= bto; x++ {
		offset := int64((x - bfrom) << uint(z.blockShift))
		blockData := data[offset : offset+z.blockSize]
		md4sum := CalcChecksum(blockData)

		if !bytes.Equal(md4sum[:z.checksumBytes], z.blockHashes[x].Checksum[:z.checksumBytes]) {
			if x > bfrom {
				// Write any good blocks we did get
				z.writeBlocks(data, bfrom, x-1)
			}
			return -1
		}
	}

	// All blocks are valid
	z.writeBlocks(data, bfrom, bto)
	return 0
}

// SubmitSourceData reads and identifies blocks of matching data
func (z *RcksumState) SubmitSourceData(data []byte, offset int64) int {
	z.mu.Lock()
	defer z.mu.Unlock()
	if z.rsumHash == nil {
		if err := z.buildHash(); err != nil {
			return 0
		}
	}

	return z.submitSourceData(data, offset)
}

// submitSourceData is the internal implementation of SubmitSourceData without locking
func (z *RcksumState) submitSourceData(data []byte, offset int64) int {
	x := 0
	gotBlocks := 0
	xLimit := len(data) - int(z.context)

	if offset != 0 {
		x = z.skip
	} else {
		z.nextMatch = nil
	}

	if x == 0 || offset == 0 {
		z.r[0] = CalcRsumBlock(data[x : x+int(z.blockSize)])
		if z.seqMatches > 1 {
			z.r[1] = CalcRsumBlock(data[x+int(z.blockSize) : x+2*int(z.blockSize)])
		}
	}
	z.skip = 0

	for x < xLimit {
		blocksMatched := 0

		// Try matching against the previously matched block's successor
		if z.nextMatch != nil && z.seqMatches > 1 {
			thismatch := z.checkChecksumsOnHashChain(z.nextMatch, data[x:], true)
			if thismatch > 0 {
				blocksMatched = 1
				gotBlocks += thismatch
			}
		}

		// Advance one byte at a time through the input
		for blocksMatched == 0 && x < xLimit {
			thismatch := z.matchBlock(data[x:], int64(offset)+int64(x))
			if thismatch > 0 {
				blocksMatched = z.seqMatches
				gotBlocks += thismatch
			}

			if blocksMatched == 0 {
				// Advance window by 1 byte - update rolling checksum
				if x+int(z.blockSize) < len(data) && x+2*int(z.blockSize) < len(data) {
					nc := data[x+int(z.blockSize)]
					Nc := data[x+2*int(z.blockSize)]
					oc := data[x]

					z.mu.Lock()
					UpdateRsum(&z.r[0], oc, nc, uint(z.blockShift))
					if z.seqMatches > 1 {
						UpdateRsum(&z.r[1], nc, Nc, uint(z.blockShift))
					}
				}
				x++
			}
		}

		// If we got a hit, skip forward by a block
		if blocksMatched > 0 {
			x += int(z.blockSize)
			if blocksMatched > 1 {
				x += int(z.blockSize)
			}

			if x <= xLimit {
				// Recalculate rolling checksums for the next blocks
				if z.seqMatches > 1 && blocksMatched == 1 {
					z.r[0] = z.r[1]
				} else if x+int(z.blockSize) <= len(data) {
					z.r[0] = CalcRsumBlock(data[x : x+int(z.blockSize)])
				}
				if z.seqMatches > 1 && x+2*int(z.blockSize) <= len(data) {
					z.r[1] = CalcRsumBlock(data[x+int(z.blockSize) : x+2*int(z.blockSize)])
				}
			}
		}
	}

	z.skip = x - xLimit

	return gotBlocks
}

// SubmitSourceFile reads a file and identifies matching blocks
func (z *RcksumState) SubmitSourceFile(f io.Reader) (int, error) {
	gotBlocks := 0
	in := int64(0)
	bufSize := int64(z.blockSize * 16)
	buf := make([]byte, bufSize+z.context)

	// Build hash tables if needed
	z.mu.Lock()
	defer z.mu.Unlock()
	if z.rsumHash == nil {
		if err := z.buildHash(); err != nil {
			return 0, err
		}
	}

	for {
		var len int
		var startIn int64 = in

		if in == 0 {
			// First read
			n, err := f.Read(buf)
			if err != nil && err != io.EOF {
				return gotBlocks, err
			}
			len = n
			in += int64(n)
		} else {
			// Move last context bytes to start, refill the rest
			copy(buf, buf[bufSize-z.context:bufSize])
			in += bufSize - z.context

			n, err := f.Read(buf[z.context:])
			if err != nil && err != io.EOF {
				return gotBlocks, err
			}
			len = int(z.context) + n
		}

		// Pad with zeros at EOF
		if len == 0 || len < int(bufSize) {
			copy(buf[len:], make([]byte, z.context))
			len += int(z.context)
		}

		// Process the buffer
		result := z.submitSourceData(buf[:len], startIn)
		gotBlocks += result

		if len < int(bufSize+z.context) {
			break
		}
	}

	return gotBlocks, nil
}

// matchBlock attempts to match a block of data at the current position
func (z *RcksumState) matchBlock(data []byte, offset int64) int {
	r0 := z.r[0]

	// Prepare hash values for lookup
	hash := uint32(r0.B)
	if z.seqMatches > 1 {
		hash ^= uint32(r0.A&z.rsumAMask) << z.hashFuncShift
	}

	// Check bithash for fast negative lookups
	bitIdx := (hash & z.bitHashMask) >> 3
	bitPos := hash & 7

	if z.bitHash != nil && int(bitIdx) < len(z.bitHash) {
		if (z.bitHash[bitIdx] & (1 << bitPos)) != 0 {
			e := z.rsumHash[hash&z.hashMask]
			if e != nil {
				return z.checkChecksumsOnHashChain(e, data, false)
			}
		}
	}

	return 0
}

// checkChecksumsOnHashChain checks data against all blocks in a hash chain
func (z *RcksumState) checkChecksumsOnHashChain(e *HashEntry, data []byte, onlyone bool) int {
	r := z.r[0]
	gotBlocks := 0

	z.nextMatch = nil

	z.rover = e
	for z.rover != nil {
		entry := z.rover
		if onlyone {
			z.rover = nil
		} else {
			z.rover = entry.Next
		}

		// Check weak checksum first
		z.stats.HashHit++

		if entry.RSum.A != (r.A&z.rsumAMask) || entry.RSum.B != r.B {
			continue
		}

		z.stats.WeakHit++

		// Calculate strong checksums
		md4sum := [2][ChecksumSize]byte{}
		donemd4 := -1

		ok := true
		for checkmd4 := 0; ok && (onlyone || checkmd4 < z.seqMatches); checkmd4++ {
			if checkmd4 > donemd4 {
				offset := checkmd4 * int(z.blockSize)
				if offset+int(z.blockSize) > len(data) {
					break
				}
				md4sum[checkmd4] = CalcChecksum(data[offset : offset+int(z.blockSize)])
				donemd4 = checkmd4
				z.stats.Checksummed++
			}

			if !bytes.Equal(md4sum[checkmd4][:z.checksumBytes], entry.Checksum[:z.checksumBytes]) {
				ok = false
			}
		}

		if ok {
			// Find the next block we already have
			blockid := z.findBlockID(entry)
			z.stats.StrongHit += donemd4 + 1

			numWriteBlocks := donemd4 + 1
			nextKnown := z.knownBlocks.nextContainedAfter(blockid + BlockID(numWriteBlocks))

			if nextKnown > blockid+BlockID(numWriteBlocks) {
				z.nextMatch = &z.blockHashes[blockid+BlockID(numWriteBlocks)]
				z.nextKnown = nextKnown
			} else {
				numWriteBlocks = int(nextKnown - blockid)
			}

			// Write the matched blocks
			z.writeBlocks(data, blockid, blockid+BlockID(numWriteBlocks-1))
			gotBlocks += numWriteBlocks
		}
	}

	return gotBlocks
}

// findBlockID finds the block ID for a hash entry
func (z *RcksumState) findBlockID(e *HashEntry) BlockID {
	for id := BlockID(0); id < BlockID(len(z.blockHashes)); id++ {
		if &z.blockHashes[id] == e {
			return id
		}
	}
	return -1
}

// writeBlocks writes a range of blocks to the output file
func (z *RcksumState) writeBlocks(data []byte, bfrom, bto BlockID) {
	if z.fd == nil {
		return
	}
	fd := z.fd
	blockShift := z.blockShift

	len := int64((bto - bfrom + 1)) << uint(blockShift)
	offset := int64(bfrom) << uint(blockShift)

	dataIdx := int64(0)
	for len > 0 {
		l := len
		if l > 0x8000000 { // 128MB chunks
			l = 0x8000000
		}

		n, err := fd.WriteAt(data[dataIdx:dataIdx+l], offset)
		if err != nil {
			return
		}

		len -= int64(n)
		dataIdx += int64(n)
		offset += int64(n)
	}

	// Mark blocks as obtained
	for id := bfrom; id <= bto; id++ {
		z.removeBlockFromHash(id)
		z.knownBlocks.addToRanges(id)
	}
}

// ReadKnownData reads back data that has already been received
func (z *RcksumState) ReadKnownData(buf []byte, offset int64) (int, error) {
	z.mu.Lock()
	defer z.mu.Unlock()
	if z.fd == nil {
		return 0, nil
	}
	fd := z.fd

	return fd.ReadAt(buf, offset)
}

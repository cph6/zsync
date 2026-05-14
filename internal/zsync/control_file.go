package zsync

/*
 * SPDX-FileCopyrightText: 2004,2005,2007,2009,2025,2026 Colin Phipps <cph@moria.org.uk>
 *
 * SPDX-License-Identifier: Artistic-2.0
 */

import (
	"bufio"
	"crypto/sha1"
	"encoding/binary"
	"fmt"
	"io"
	"os"
	"strconv"
	"strings"
	"time"

	"github.com/cph6/zsync/internal/rcksum"
)

// ZsyncBegin loads a zsync file and returns the state tracking object.
func Begin(f *os.File) (*State, error) {
	checksumBytes := 16
	rsumBytes := 4
	seqMatches := 1

	zs := &State{}

	safelines := []string{}

	reader := bufio.NewReader(f)
	for {
		line, err := reader.ReadString('\n')
		if err != nil {
			return nil, err
		}
		line = strings.TrimSuffix(line, "\n")
		if line == "" {
			break
		}

		parts := strings.SplitN(line, ": ", 2)
		if len(parts) != 2 {
			return nil, fmt.Errorf("bad line: %s", line)
		}
		key, value := parts[0], parts[1]

		switch key {
		case "zsync":
			if value == "0.0.4" {
				return nil, fmt.Errorf("not compatible with zsync 0.0.4")
			}
		case "Min-Version":
			// Assume version is ok
		case "Length":
			var err error
			zs.filelen, err = strconv.ParseInt(value, 10, 64)
			if err != nil {
				return nil, fmt.Errorf("bad length: %s", value)
			}
		case "Filename":
			zs.filename = value
		case "URL":
			zs.urls = append(zs.urls, value)
		case "Blocksize":
			blocksize, err := strconv.ParseInt(value, 10, 64)
			if err != nil {
				return nil, fmt.Errorf("bad blocksize: %s", value)
			}
			if blocksize&(blocksize-1) != 0 {
				return nil, fmt.Errorf("nonsensical blocksize %d", blocksize)
			}
			zs.blocksize = blocksize
		case "Hash-Lengths":
			parts := strings.Split(value, ",")
			if len(parts) != 3 {
				return nil, fmt.Errorf("bad hash lengths: %s", value)
			}
			var err error
			seqMatches, err = strconv.Atoi(parts[0])
			if err != nil {
				return nil, fmt.Errorf("bad seqMatches: %s", parts[0])
			}
			rsumBytes, err = strconv.Atoi(parts[1])
			if err != nil {
				return nil, fmt.Errorf("bad rsumBytes: %s", parts[1])
			}
			checksumBytes, err = strconv.Atoi(parts[2])
			if err != nil {
				return nil, fmt.Errorf("bad checksumBytes: %s", parts[2])
			}
			if rsumBytes < 1 || rsumBytes > 4 || checksumBytes < 3 || checksumBytes > 16 || seqMatches < 1 || seqMatches > 2 {
				return nil, fmt.Errorf("nonsensical hash lengths: %s", value)
			}
		case "Safe":
			safelines = append(safelines, value)
		case "SHA-1":
			if len(value) != sha1.Size*2 {
				return nil, fmt.Errorf("SHA-1 digest wrong length")
			}
			zs.checksum = value
			zs.checksumMethod = "SHA-1"
		case "MTime":
			t, err := time.Parse(time.RFC1123Z, value)
			if err != nil {
				return nil, fmt.Errorf("bad mtime: %s", value)
			}
			zs.mtime = t
		default:
			// Ignore headers if they were included in "Safe".
			for _, safe := range safelines {
				if safe == key {
					break
				}
			}
			// Otherwise reject unknown header.
			return nil, fmt.Errorf("unknown header: %s", key)
		}
	}

	if zs.filelen != 0 && zs.blocksize != 0 {
		zs.blocks = (zs.filelen + zs.blocksize - 1) / zs.blocksize
	}

	if zs.filelen == 0 || zs.blocksize == 0 {
		return nil, fmt.Errorf("not a zsync file")
	}

	rs, err := rcksum.New(rcksum.BlockID(zs.blocks), zs.blocksize, int(rsumBytes), uint(checksumBytes), seqMatches)
	if err != nil {
		return nil, err
	}
	zs.Rs = rs

	// Read block checksums
	for i := int64(0); i < zs.blocks; i++ {
		var rsum rcksum.RSum
		rsumByte := make([]byte, 4)
		checksum := make([]byte, checksumBytes)

		// Read rsum.
		if _, err := io.ReadFull(reader, rsumByte[4-rsumBytes:]); err != nil {
			return nil, err
		}
		_, err = binary.Decode(rsumByte, binary.BigEndian, &rsum)
		if err != nil {
			return nil, err
		}

		// Read checksum
		if _, err := io.ReadFull(reader, checksum); err != nil {
			return nil, err
		}

		var cksum [rcksum.ChecksumSize]byte
		copy(cksum[:], checksum)
		rs.AddTargetBlock(rcksum.BlockID(i), rsum, cksum)
	}

	return zs, nil
}

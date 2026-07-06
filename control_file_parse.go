package zsync

/*
 * SPDX-FileCopyrightText: 2004,2005,2007,2009,2025,2026 Colin Phipps <cph@moria.org.uk>
 *
 * SPDX-License-Identifier: Artistic-2.0
 */

import (
	"bufio"
	"crypto"
	"crypto/sha1"
	_ "crypto/sha256"
	"encoding/binary"
	"errors"
	"fmt"
	"io"
	"strconv"
	"strings"
	"time"

	"github.com/cph6/zsync/internal/rcksum"
)

var (
	ErrNotZsyncFile           = errors.New("not a zsync file")
	ErrMissingRequiredFields  = errors.New("required fields missing from zsync control file")
	ErrNotSupportedCompressed = errors.New("zsync file only offers compressed download options - not supported in this version")
	ErrNotSupported004        = errors.New("zsync 0.0.4 control files are not supported")
)

const (
	sizeofStructGzblock = 4 // from zsync-0.6.x
)

func (zs *Syncer) parseControlFile(f io.Reader) error {
	checksumBytes := 16
	rsumBytes := 4
	seqMatches := 1

	// Set of headers that we can ignore safely, and counts of how often they
	// were seen.
	safelines := make(map[string]int)
	safelines["Z-URL"] = 0
	safelines["Min-Version"] = 0
	seenHeader := false

	reader := bufio.NewReader(f)
	for {
		line, err := reader.ReadString('\n')
		if err != nil {
			return err
		}
		line = strings.TrimSuffix(line, "\n")
		if line == "" {
			break
		}

		parts := strings.SplitN(line, ": ", 2)
		if len(parts) != 2 {
			return fmt.Errorf("bad line: %s", line)
		}
		key, value := parts[0], parts[1]

		// Require the "zsync:" header to come first.
		if !seenHeader {
			if key != "zsync" {
				return ErrNotZsyncFile
			}
			if value == "0.0.4" {
				return ErrNotSupported004
			}
			seenHeader = true
			continue
		}

		switch key {
		case "Length":
			var err error
			zs.filelen, err = strconv.ParseInt(value, 10, 64)
			if err != nil {
				return fmt.Errorf("bad length: %s", value)
			}
		case "Filename":
			zs.targetFilename = value
		case "URL":
			zs.urls = append(zs.urls, value)
		case "Blocksize":
			blocksize, err := strconv.ParseInt(value, 10, 64)
			if err != nil {
				return fmt.Errorf("bad blocksize: %s", value)
			}
			if blocksize&(blocksize-1) != 0 {
				return fmt.Errorf("nonsensical blocksize %d", blocksize)
			}
			zs.blocksize = blocksize
		case "Hash-Lengths":
			parts := strings.Split(value, ",")
			if len(parts) != 3 {
				return fmt.Errorf("bad hash lengths: %s", value)
			}
			var err error
			seqMatches, err = strconv.Atoi(parts[0])
			if err != nil {
				return fmt.Errorf("bad seqMatches: %s", parts[0])
			}
			rsumBytes, err = strconv.Atoi(parts[1])
			if err != nil {
				return fmt.Errorf("bad rsumBytes: %s", parts[1])
			}
			checksumBytes, err = strconv.Atoi(parts[2])
			if err != nil {
				return fmt.Errorf("bad checksumBytes: %s", parts[2])
			}
			if rsumBytes < 1 || rsumBytes > 4 || checksumBytes < 3 || checksumBytes > 16 || seqMatches < 1 || seqMatches > 2 {
				return fmt.Errorf("nonsensical hash lengths: %s", value)
			}
		case "Safe":
			safelines[value] = 0
		case "File-Hash":
			vs := strings.SplitN(value, ":", 2)
			alg := vs[0]
			if len(vs) == 2 {
				switch alg {
				case "SHA-1":
					zs.checksumMethod = crypto.SHA1
					zs.checksum = vs[1]
				case "SHA-256":
					zs.checksumMethod = crypto.SHA256
					zs.checksum = vs[1]
				}
			}
			// Else, proceed without verification.
		case "SHA-1":
			if len(value) != sha1.Size*2 {
				return fmt.Errorf("SHA-1 digest wrong length")
			}
			zs.checksum = value
			zs.checksumMethod = crypto.SHA1
		case "MTime":
			t, err := time.Parse(time.RFC1123Z, value)
			if err != nil {
				return fmt.Errorf("bad mtime: %s", value)
			}
			zs.mtime = t
		case "Z-Map2":
			// zsync-0.7.0 and onwards does not support downloading required data
			// from inside a compressed version of that data. (It supports
			// downloading gzip --rsyncable compressed data where the compressed data
			// is the target.)
			// But a zsync-0.6.x control file can support uncompressed and compressed
			// downloads, so the client library here is capable of *skipping* a Z-Map2
			// header.
			numEntries, err := strconv.ParseInt(value, 10, 64)
			if err != nil {
				return fmt.Errorf("bad Z-Map2 size: %s", value)
			}
			buf := make([]byte, numEntries*sizeofStructGzblock)
			if _, err := io.ReadFull(reader, buf); err != nil {
				return err
			}
			_ = buf
		default:
			// Ignore headers if they were included in "Safe".
			if _, ok := safelines[key]; ok {
				safelines[key]++
			} else {
				return fmt.Errorf("unknown header: %s", key)
			}
		}
	}

	if zs.filelen == 0 || zs.blocksize == 0 {
		return ErrMissingRequiredFields
	}
	zs.blocks = (zs.filelen + zs.blocksize - 1) / zs.blocksize

	// The current client does not support any file without a "URL" provided; but
	// someone using the library might provide alternative download mechanisms so
	// we do not treat all files without a URL line as unsupported here. However
	// any zsync-0.6.x control file with a compressed URL is clearly intended to
	// be used with HTTP downloading, which is not supported by zsync-0.7.0 and
	// above unless a non-compressed alternative URL was given.
	if len(zs.urls) == 0 && safelines["Z-URL"] > 0 {
		return ErrNotSupportedCompressed
	}

	rs, err := rcksum.New(rcksum.BlockID(zs.blocks), zs.blocksize, int(rsumBytes), uint(checksumBytes), seqMatches)
	if err != nil {
		return err
	}
	zs.rs = rs

	// Read block checksums
	for i := int64(0); i < zs.blocks; i++ {
		var rsum rcksum.RSum
		rsumByte := make([]byte, 4)
		checksum := make([]byte, checksumBytes)

		// Read rsum.
		if _, err := io.ReadFull(reader, rsumByte[4-rsumBytes:]); err != nil {
			return err
		}
		_, err = binary.Decode(rsumByte, binary.BigEndian, &rsum)
		if err != nil {
			return err
		}

		// Read checksum
		if _, err := io.ReadFull(reader, checksum); err != nil {
			return err
		}

		var cksum [rcksum.ChecksumSize]byte
		copy(cksum[:], checksum)
		zs.rs.AddTargetBlock(rcksum.BlockID(i), rsum, cksum)
	}
	return nil
}

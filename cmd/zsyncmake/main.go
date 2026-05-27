package main

/*
 * SPDX-FileCopyrightText: 2004,2005,2007,2009,2025,2026 Colin Phipps <cph@moria.org.uk>
 *
 * SPDX-License-Identifier: Artistic-2.0
 */

// AI: copilot / Claude Haiku conversion of zsync's make.c.

import (
	"bufio"
	"bytes"
	"crypto/sha1"
	"encoding/binary"
	"encoding/hex"
	"flag"
	"fmt"
	"io"
	"math"
	"os"
	"path/filepath"
	"time"

	"github.com/cph6/zsync/internal/rcksum"
)

const (
	version = "0.7.1"
)

func main() {
	// Command-line flags
	blocksize := flag.Int64("b", 0, "block size (must be power of 2)")
	outputFile := flag.String("o", "", "filename for the created .zsync file")
	filename := flag.String("f", "", "recommended filename clients should use for the target file")
	verbose := flag.Bool("v", false, "verbose output")
	urls := make([]string, 0)

	// Custom flag handling for -u URL
	flag.Func("u", "URL to include in .zsync", func(s string) error {
		urls = append(urls, s)
		return nil
	})

	flag.Bool("e", false, "no-op (for compatibility)")
	flag.Bool("z", false, "no-op (for compatibility)")
	flag.Bool("Z", false, "no-op (for compatibility)")

	showVersion := flag.Bool("V", false, "show version")

	flag.Parse()

	if *showVersion {
		fmt.Printf("zsyncmake v%s\nBy Colin Phipps <cph@moria.org.uk>\nPublished under the Artistic License 2.0, see the LICENSE file for details.\n", version)
		os.Exit(0)
	}

	// Get input file
	args := flag.Args()
	var inputFile string
	var instream io.Reader
	var fileHandle *os.File
	var err error
	var fileInfo os.FileInfo
	var isStdin bool

	switch len(args) {
	case 1:
		inputFile = args[0]
		fileHandle, err = os.Open(inputFile)
		if err != nil {
			fmt.Fprintf(os.Stderr, "open: %v\n", err)
			os.Exit(2)
		}

		// Get file stats.
		fileInfo, _ = fileHandle.Stat()
		// On error, we just continue without stat information.

		if *filename == "" {
			*filename = filepath.Base(inputFile)
		}
		instream = fileHandle
		isStdin = false
	case 0:
		instream = os.Stdin
		isStdin = true
	default:
		fmt.Fprintf(os.Stderr, "usage: zsyncmake [options] [inputfile]\n")
		flag.PrintDefaults()
		os.Exit(2)
	}

	if *blocksize == 0 {
		// fileInfo.Length might be zero if we do not have file stats;
		// defaulting to 2048 in that case.
		if fileInfo.Size() < int64(100000000) {
			*blocksize = 2048
		} else {
			*blocksize = 4096
		}
	}

	// Validate blocksize is power of 2
	if (*blocksize & (*blocksize - 1)) != 0 {
		fmt.Fprintf(os.Stderr, "blocksize must be a power of 2 (512, 1024, 2048, ...)\n")
		os.Exit(2)
	}

	fileLen, sha1sum, checksumFile, err := readFileCalcChecksumsAndStats(instream, *blocksize)
	if err != nil {
		fmt.Fprintf(os.Stderr, "error reading file: %v\n", err)
		os.Exit(2)
	}
	defer func() {
		err := os.Remove(checksumFile.Name())
		if err != nil {
			fmt.Fprintf(os.Stderr, "failed to remove temporary file %s\n", checksumFile.Name())
			// And disregard - this does not affect the generated output.
		}
	}()
	if !isStdin {
		err := fileHandle.Close()
		if err != nil {
			fmt.Fprintf(os.Stderr, "failed to finish reading file: %v\n", err)
			os.Exit(2)
		}
	}

	seqMatches, rsumLen, checksumLen := determineHashLengths(fileLen, *blocksize)

	// Prepare output filename
	outName := *outputFile
	if outName == "" && *filename != "" {
		outName = *filename + ".zsync"
	}

	// Open output file
	var outstream *os.File
	if outName != "" {
		var err error
		outstream, err = os.Create(outName)
		if err != nil {
			fmt.Fprintf(os.Stderr, "open: %v\n", err)
			os.Exit(2)
		}
	} else {
		outstream = os.Stdout
	}

	if len(urls) == 0 && inputFile != "" {
		urls = []string{inputFile}
		fmt.Fprintf(os.Stderr, "No URL given, so I am including a relative URL in the .zsync file - you must keep the file being served and the .zsync in the same public directory. Use -u %s to get this same result without this warning.\n", inputFile)
	}

	err = writeControlFile(outstream, *filename, fileLen, urls, fileInfo.ModTime(), *blocksize, rsumLen, checksumLen, seqMatches, sha1sum, checksumFile)
	if err != nil {
		fmt.Fprintf(os.Stderr, "failed writing zsync file: %v\n", err)
		os.Exit(2)
	}
	err = outstream.Close()
	if err != nil {
		fmt.Fprintf(os.Stderr, "failed finalising zsync file: %v\n", err)
		os.Exit(2)
	}
	if *verbose {
		fmt.Fprintf(os.Stderr, "Created .zsync file with %d blocks\n", fileLen / *blocksize)
	}
}

func writeControlFile(outstream io.Writer, filename string, fileLen int64, urls []string, mtime time.Time, blocksize int64, rsumLen, checksumLen, seqMatches int, sha1sum []byte, checksumFile io.Reader) error {
	// Write .zsync file
	writer := bufio.NewWriter(outstream)

	_, err := fmt.Fprintf(writer, "zsync: %s\n", version)
	if err != nil {
		return err
	}

	if filename != "" {
		_, err = fmt.Fprintf(writer, "Filename: %s\n", filename)
		if err != nil {
			return err
		}
		if !mtime.IsZero() {
			_, err = fmt.Fprintf(writer, "MTime: %s\n", mtime.UTC().Format(time.RFC1123Z))
			if err != nil {
				return err
			}
		}
	}

	_, err = fmt.Fprintf(writer, `Blocksize: %d
Length: %d
Hash-Lengths: %d,%d,%d
SHA-1: %s
`, blocksize, fileLen,seqMatches, rsumLen, checksumLen,hex.EncodeToString(sha1sum))
if err != nil {
	return err
}


	// Write URLs
	for _, url := range urls {
		_, err = fmt.Fprintf(writer, "URL: %s\n", url)
		if err != nil {
			return err
		}
	}

	// End of headers
	_, err = fmt.Fprintf(writer, "\n")
	if err != nil {
		return err
	}
	err = writeChecksums(writer, checksumFile, blocksize, rsumLen, checksumLen, seqMatches)
	if err != nil {
		return err
	}
	err = writer.Flush()
	if err != nil {
		return err
	}
	return nil
}

func determineHashLengths(fileLengthInt, blocksizeInt int64) (int, int, int) {
	length := float64(fileLengthInt)
	blocksize := float64(blocksizeInt)
	seqMatches := 1
	rsumLen := int(math.Ceil(((math.Log2(length) + math.Log2(blocksize)) - 8.6) / 8))

	if rsumLen > 4 {
		seqMatches = 2
		rsumLen = 4
	}
	if rsumLen < 2 {
		rsumLen = 2
	}

	// Calculate checksum length
	checksumLen := int(math.Max(
		math.Ceil((20+math.Log2(length)+math.Log2(1+length/blocksize))/float64(seqMatches)/8),
		math.Ceil((20+math.Log2(1+length/blocksize))/8),
	))
	checksumLen = min(16, max(4, checksumLen))
	return seqMatches, rsumLen, checksumLen
}

// Temporary structure for holding checksums of a block during processing.
type blockChecksums struct {
	Rsum rcksum.RSum
	MD4  [16]byte
}

func readFileCalcChecksumsAndStats(r io.Reader, blocksize int64) (int64, []byte, *os.File, error) {
	// Create temporary buffer for reading blocks
	buffer := make([]byte, blocksize)

	// SHA-1 context
	sha1Hash := sha1.New()
	fileLen := int64(0)

	tempFile, err := os.CreateTemp("", "zsyncmake-*")
	if err != nil {
		return 0, nil, nil, fmt.Errorf("create temp file: %v", err)
	}

	writer := bufio.NewWriter(tempFile)

	// Read file in blocks, calculate checksums, and write to temp file
	for {
		n, err := io.ReadFull(r, buffer)
		if err != nil && err != io.EOF && err != io.ErrUnexpectedEOF {
			return 0, nil, nil, fmt.Errorf("read: %v", err)
		}
		if n > 0 {
			fileLen += int64(n)

			// Add to SHA-1. SHA-1 is calculated on the actual file data,
			// not the padded blocks, so only add the bytes read.
			sha1Hash.Write(buffer[:n])

			// Now pad to blocksize if needed for checksum calculation.
			if n < int(blocksize) {
				padding := bytes.Repeat([]byte{0}, int(blocksize)-n)
				buffer = append(buffer[:n], padding...)
			}

			// Calculate checksums on the data.
			checksums := &blockChecksums{
				Rsum: rcksum.CalcRsumBlock(buffer),
				MD4:  rcksum.CalcChecksum(buffer),
			}

			// And write out to the temp file; these will be read back for
			// truncation and writing to the .zsync file later.
			err := binary.Write(writer, binary.BigEndian, checksums)
			if err != nil {
				return 0, nil, nil, fmt.Errorf("write temp file: %v", err)
			}
		}
		if err == io.EOF || err == io.ErrUnexpectedEOF {
			break
		}
	}
	err = writer.Flush()
	if err != nil {
		return 0, nil, nil, fmt.Errorf("write temp file: %v", err)
	}
	_, err = tempFile.Seek(0, io.SeekStart)
	if err != nil {
		return 0, nil, nil, fmt.Errorf("seek temp file: %v", err)
	}

	return fileLen, sha1Hash.Sum(nil), tempFile, nil
}

func writeChecksums(writer io.Writer, checksumFile io.Reader, blocksize int64, rsumLen int, checksumLen int, seqMatches int) error {
	// Write block checksums
	for {
		var checksums blockChecksums
		err := binary.Read(checksumFile, binary.BigEndian, &checksums)
		if err != nil {
			if err == io.EOF {
				break
			}
			return fmt.Errorf("read temp file: %v", err)
		}

		// Write rsum (network byte order, truncated to rsumLen)
		rsumBytes := make([]byte, 4)
		_, err = binary.Encode(rsumBytes, binary.BigEndian, checksums.Rsum)
		if err != nil {
			return fmt.Errorf("encode: %v", err)
		}
		_, err = writer.Write(rsumBytes[4-rsumLen:])
		if err != nil {
			return fmt.Errorf("write: %v", err)
		}

		// Write checksum (truncated to checksumLen)
		_, err = writer.Write(checksums.MD4[:checksumLen])
		if err != nil {
			return fmt.Errorf("write: %v", err)
		}
	}

	return nil
}

func max(a, b int) int {
	if a > b {
		return a
	}
	return b
}

func min(a, b int) int {
	if a < b {
		return a
	}
	return b
}

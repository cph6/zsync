package zsync_test

/*
 * SPDX-FileCopyrightText: 2026 Colin Phipps <cph@moria.org.uk>
 *
 * SPDX-License-Identifier: Artistic-2.0
 */
// AI: copilot / GPT-5 mini, with significant refactoring.

import (
	"bytes"
	"encoding/binary"
	"fmt"
	"io"
	"net/http"
	"os"
	"path/filepath"
	"testing"

	"github.com/cph6/zsync"
	"github.com/cph6/zsync/internal/rcksum"
)

// fakeClient implements zsync.HTTPRequester for tests. It always returns the
// provided status code and body for any request.
type fakeClient struct {
	status int
	body   []byte
}

func (f *fakeClient) Do(req *http.Request) (*http.Response, error) {
	return &http.Response{
		StatusCode: f.status,
		Body:       io.NopCloser(bytes.NewReader(f.body)),
		Header:     make(http.Header),
	}, nil
}

func MakeZsyncControl(data [][]byte) bytes.Buffer {
	blocksize := len(data[0])

	// Build control file: headers then binary checksums
	var buf bytes.Buffer
	buf.WriteString("zsync: 0.7.0\n")
	fmt.Fprintf(&buf, "Length: %d\n", (len(data)-1)*blocksize+len(data[len(data)-1]))
	fmt.Fprintf(&buf, "Blocksize: %d\n", blocksize)
	buf.WriteString("Hash-Lengths: 1,4,16\n")
	buf.WriteString("Filename: testfile\n")
	buf.WriteString("URL: http://example.com/foo\n")
	buf.WriteString("\n")

	for _, d := range data {
		// compute rsum and md4
		rsum := rcksum.CalcRsumBlock(d)
		md4sum := rcksum.CalcChecksum(d)

		// write rsum as big-endian two uint16
		var rsumBuf [4]byte
		binary.Encode(rsumBuf[:4], binary.BigEndian, rsum)
		buf.Write(rsumBuf[:4])
		// write md4
		buf.Write(md4sum[:])
	}
	return buf
}

// TestNewAndEnd verifies that zsync.New can parse a minimal control file
// and that End closes the temporary file and optionally renames it.
func TestNewAndEnd(t *testing.T) {
	// Create a one-block file control file.
	data := []byte{1, 2, 3, 4, 5, 6, 7, 8}
	control := MakeZsyncControl([][]byte{data})

	dir := t.TempDir()
	s, err := zsync.New(bytes.NewReader(control.Bytes()), filepath.Join(dir, "outfile"))
	if err != nil {
		t.Fatalf("New failed: %v", err)
	}
	// End without renaming should close the temp file and return its name
	fname, err := s.End("")
	if err != nil {
		t.Fatalf("End failed: %v", err)
	}
	// file should exist
	if _, err := os.Stat(fname); err != nil {
		t.Fatalf("expected temp file to exist: %v", err)
	}
}

// TestFetchRemainingBlocks exercises the high-level FetchRemainingBlocks
// method using a fake HTTPRequester that returns the exact block data.
func TestFetchRemainingBlocks(t *testing.T) {
	// Setup control file for a single block
	data := []byte{10, 11, 12, 13, 14, 15, 16, 17}
	control := MakeZsyncControl([][]byte{data})

	dir := t.TempDir()
	s, err := zsync.New(bytes.NewReader(control.Bytes()), filepath.Join(dir, "outfile"))
	if err != nil {
		t.Fatalf("New failed: %v", err)
	}

	// replace URL list with a synthetic URL (doesn't matter for fake client)
	// Use the fake client which returns 206 Partial Content and our block data
	client := &fakeClient{status: http.StatusPartialContent, body: data}

	// Fetch remaining blocks. We provide empty referer and noProgress=true
	got, err := s.FetchRemainingBlocks(client, "", nil)
	if err != nil {
		t.Fatalf("FetchRemainingBlocks failed: %v", err)
	}
	if got == 0 {
		t.Fatalf("expected some bytes downloaded")
	}
	if s.Status() != zsync.CompleteData {
		t.Fatalf("expected Syncer to be complete, got status %d", s.Status())
	}
	// Verify file contents
	fname := s.Filename()
	b, err := os.ReadFile(fname)
	if err != nil {
		t.Fatalf("read file: %v", err)
	}
	if !bytes.Equal(b[:len(data)], data) {
		t.Fatalf("file contents differ")
	}
}

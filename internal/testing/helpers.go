package testing

/*
 * SPDX-FileCopyrightText: 2026 Colin Phipps <cph@moria.org.uk>
 *
 * SPDX-License-Identifier: Artistic-2.0
 */

// AI: copilot / Claude Haiku

import (
	"bytes"
	"crypto/md5"
	"fmt"
	"io"
	"math/rand"
	"net"
	"os"
	"path/filepath"
	"testing"
	"time"
)

// WriteTestFile writes a test file with a given length
// and contents derived deterministically from a random
// seed.
func WriteTestFile(filePath string, length int, seed int64) error {
	rnd := rand.New(rand.NewSource(seed))
	f, err := os.Create(filePath)
	if err != nil {
		return err
	}
	defer f.Close()
	for length > 0 {
		n := 4096
		if n > length {
			n = length
		}
		buf := make([]byte, n)
		n, err := rnd.Read(buf)
		if err != nil {
			return err
		}
		_, err = f.Write(buf)
		if err != nil {
			return err
		}
		length -= n
	}
	return nil
}

// ProvideSeed creates a partial seed file from source.
func ProvideSeed(t *testing.T, scratchDir, source string, length int) string {
	seedFile := filepath.Join(scratchDir, fmt.Sprintf("seed-%d", time.Now().UnixNano()))

	srcFile, err := os.Open(source)
	if err != nil {
		t.Fatalf("Failed to open source file: %v", err)
	}
	defer srcFile.Close()

	dstFile, err := os.Create(seedFile)
	if err != nil {
		t.Fatalf("Failed to create seed file: %v", err)
	}
	defer dstFile.Close()

	// Write portion from position length/10 to length/10 + length/5
	srcFile.Seek(int64(length/10), 0)
	if _, err := io.CopyN(dstFile, srcFile, int64(length/5)); err != nil && err != io.EOF {
		t.Fatalf("Failed to copy seed data: %v", err)
	}

	// Write portion from position length/3 to length/3 + length/3
	srcFile.Seek(int64(length/3), 0)
	if _, err := io.CopyN(dstFile, srcFile, int64(length/3)); err != nil && err != io.EOF {
		t.Fatalf("Failed to copy seed data: %v", err)
	}

	return seedFile
}

// HexMD5 calculates MD5 hash of a file and returns hex string
func HexMD5(filePath string) (string, error) {
	file, err := os.Open(filePath)
	if err != nil {
		return "", err
	}
	defer file.Close()

	md5hash := md5.New()
	if _, err := io.Copy(md5hash, file); err != nil {
		return "", err
	}

	return fmt.Sprintf("%x", md5hash.Sum(nil)), nil
}

// CopyFile copies a file from src to dst
func CopyFile(src, dst string) error {
	srcFile, err := os.Open(src)
	if err != nil {
		return err
	}
	defer srcFile.Close()

	dstFile, err := os.Create(dst)
	if err != nil {
		return err
	}
	defer dstFile.Close()

	_, err = io.Copy(dstFile, srcFile)
	return err
}

// AssertFilesEqual checks if two files are identical
func AssertFilesEqual(t *testing.T, file1, file2 string) {
	stat1, err := os.Stat(file1)
	if err != nil {
		t.Fatalf("Failed to stat %s: %v", file1, err)
	}

	stat2, err := os.Stat(file2)
	if err != nil {
		t.Fatalf("Failed to stat %s: %v", file2, err)
	}

	if stat1.Size() != stat2.Size() {
		t.Errorf("File sizes differ: %d vs %d", stat1.Size(), stat2.Size())
		return
	}

	if int64(stat1.ModTime().Unix()) != int64(stat2.ModTime().Unix()) {
		t.Logf("Warning: modification times differ: %d vs %d", stat1.ModTime().Unix(), stat2.ModTime().Unix())
	}

	f1, err := os.Open(file1)
	if err != nil {
		t.Fatalf("Failed to open %s: %v", file1, err)
	}
	defer f1.Close()

	f2, err := os.Open(file2)
	if err != nil {
		t.Fatalf("Failed to open %s: %v", file2, err)
	}
	defer f2.Close()

	buf1 := make([]byte, 4096)
	buf2 := make([]byte, 4096)

	for {
		n1, err1 := f1.Read(buf1)
		n2, err2 := f2.Read(buf2)

		if n1 != n2 || !bytes.Equal(buf1[:n1], buf2[:n2]) {
			t.Errorf("File contents differ")
			return
		}

		if err1 != nil || err2 != nil {
			break
		}
	}
}

// GetLocalIP returns the local machine's IP address (excluding localhost)
// It tries to connect to a remote address to determine the local interface
func GetLocalIP() string {
	// Try to connect to a public DNS server to determine local interface
	// This doesn't actually send any data, just determines which interface would be used
	conn, err := net.Dial("udp", "8.8.8.8:80")
	if err != nil {
		return ""
	}
	defer conn.Close()

	localAddr := conn.LocalAddr().(*net.UDPAddr)
	return localAddr.IP.String()
}

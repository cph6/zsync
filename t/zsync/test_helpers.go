package main

import (
	"bytes"
	"crypto/md5"
	"fmt"
	"io"
	"math/rand"
	"os"
	"testing"
)

func writeTestFile(filePath string, length int, seed int64) error {
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

// hexMD5 calculates MD5 hash of a file and returns hex string
func hexMD5(filePath string) (string, error) {
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

// copyFile copies a file from src to dst
func copyFile(src, dst string) error {
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

// assertFilesEqual checks if two files are identical
func assertFilesEqual(t *testing.T, file1, file2 string) {
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

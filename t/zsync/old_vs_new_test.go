package main

import (
	"fmt"
	"os"
	"path/filepath"
	"testing"

	zstesting "github.com/cph6/zsync/internal/testing"
)

// Test06ControlFileWithCurrentClient
func Test06ControlFileWithCurrentClient(t *testing.T) {
	if testing.Short() {
		t.Skip("Skipping integration test in short mode")
	}

	seedFile := zstesting.ProvideSeed(t, scratchDir, filepath.Join(testDataDir, targetFile), 281020)
	defer os.Remove(seedFile)

	outfile1, _, _, _ := tryZSync(t, targetFile+".0.6.zsync", tryZSyncOptions{
		Parameters: []string{"-i", seedFile},
	})
	defer os.Remove(outfile1)

	zstesting.AssertFilesEqual(t, outfile1, filepath.Join(testDataDir, targetFile))
}

// TestCurrentControlFileWith06Client
func TestCurrentControlFileWith06Client(t *testing.T) {
	if testing.Short() {
		t.Skip("Skipping integration test in short mode")
	}
	binary := filepath.Join(binaryDir, "zsync-0.6")
	if _, err := os.Stat(binary); os.IsNotExist(err) {
		t.Skip("no zsync-0.6 available")
	}

	targetPath := filepath.Join(testDataDir, targetFile)
	controlFile := makeZSync(t, targetPath)
	defer os.Remove(controlFile)
	fmt.Fprintf(os.Stderr, "control file: %s\n", controlFile)

	seedFile := zstesting.ProvideSeed(t, scratchDir, filepath.Join(testDataDir, targetFile), 281020)
	defer os.Remove(seedFile)

	outfile1, _, _, _ := tryZSync(t, "test.zsync", tryZSyncOptions{
		Parameters: []string{"-i", seedFile},
		Binary:     binary,
		URL:        zstesting.HttpURL,
	})
	defer os.Remove(outfile1)

	zstesting.AssertFilesEqual(t, outfile1, filepath.Join(testDataDir, targetFile))
}

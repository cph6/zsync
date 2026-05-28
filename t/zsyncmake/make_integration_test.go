package main

import (
	"bufio"
	"bytes"
	"fmt"
	"io"
	"os"
	"os/exec"
	"path/filepath"
	"strings"
	"testing"
)

var (
	binaryDir   = "../.."
	testDataDir = "../data"
	scratchDir  string
)

// TestMain sets up and tears down the test environment
func TestMain(m *testing.M) {
	setup()
	code := m.Run()
	teardown()
	os.Exit(code)
}

func setup() {
	// Create scratch directory
	var err error
	if scratchDir, err = os.MkdirTemp("", "zsync-test-"); err != nil {
		panic(fmt.Sprintf("Failed to create scratch directory: %v", err))
	}
}

func teardown() {
	// Clean up scratch directory
	if strings.HasPrefix(filepath.Base(scratchDir), "zsync-test-") {
		os.RemoveAll(scratchDir)
	}
}

// parseZSyncFile parses a zsync file and returns the headers and block data
func parseZSyncFile(filePath string) (map[string][]byte, []byte, error) {
	file, err := os.Open(filePath)
	if err != nil {
		return nil, nil, err
	}
	defer file.Close()

	info := make(map[string][]byte)
	scanner := bufio.NewScanner(file)

	// Parse headers until empty line
	for scanner.Scan() {
		line := scanner.Bytes()
		if len(line) == 0 {
			break
		}

		parts := bytes.SplitN(line, []byte(": "), 2)
		if len(parts) != 2 {
			continue
		}

		key := string(parts[0])
		value := parts[1]

		// Special handling for Z-Map2
		if key == "Z-Map2" {
			mapSize := 0
			fmt.Sscanf(string(value), "%d", &mapSize)
			mapData := make([]byte, 4*mapSize)
			if _, err := io.ReadFull(file, mapData); err != nil {
				return nil, nil, err
			}
			info[key] = mapData
		} else {
			info[key] = value
		}
	}

	// Read remaining data as block data
	blockData, err := io.ReadAll(file)
	if err != nil {
		return nil, nil, err
	}

	return info, blockData, nil
}

// makeZSync creates a zsync file using the zsyncmake command
func makeZSync(t *testing.T, filename string, params ...string) string {
	outfile := filepath.Join(scratchDir, "test.zsync")

	args := []string{"-o", outfile}
	args = append(args, params...)
	args = append(args, filename)

	cmd := exec.Command(filepath.Join(binaryDir, "zsyncmake"), args...)

	output, err := cmd.CombinedOutput()
	if err != nil {
		t.Fatalf("zsyncmake failed: %v\nOutput: %s", err, output)
	}

	return outfile
}

// TestZSyncMakeSimple tests basic zsync file creation
func TestZSyncMakeSimple(t *testing.T) {
	if testing.Short() {
		t.Skip("Skipping integration test in short mode")
	}

	zsyncFile := makeZSync(t, filepath.Join(testDataDir, "target.dat"), "-u", "target.dat", "-Z")
	defer os.Remove(zsyncFile)

	info, _, err := parseZSyncFile(zsyncFile)
	if err != nil {
		t.Fatalf("Failed to parse zsync file: %v", err)
	}

	expected := map[string]string{
		"Filename": "target.dat",
		"Length":   "281020",
		"URL":      "target.dat",
		"SHA-1":    "d0be479e0a823100bd09a997d125979626272453",
	}

	for key, expectedVal := range expected {
		actualVal := string(info[key])
		if actualVal != expectedVal {
			t.Errorf("Expected %s=%s, got %s", key, expectedVal, actualVal)
		}
	}

	// Check that Z-Map2 is not present
	if _, ok := info["Z-Map2"]; ok {
		t.Error("Z-Map2 should not be present in output")
	}
}

// TestZSyncMakeEmpty tests zsync creation with empty file
func TestZSyncMakeEmpty(t *testing.T) {
	if testing.Short() {
		t.Skip("Skipping integration test in short mode")
	}

	zsyncFile := makeZSync(t, filepath.Join(testDataDir, "empty"))
	defer os.Remove(zsyncFile)

	info, _, err := parseZSyncFile(zsyncFile)
	if err != nil {
		t.Fatalf("Failed to parse zsync file: %v", err)
	}

	if string(info["Length"]) != "0" {
		t.Errorf("Expected Length=0, got %s", info["Length"])
	}

	if string(info["SHA-1"]) != "da39a3ee5e6b4b0d3255bfef95601890afd80709" {
		t.Errorf("Unexpected SHA-1: %s", info["SHA-1"])
	}
}

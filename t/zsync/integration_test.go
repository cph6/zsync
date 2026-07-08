package main

/*
 * SPDX-FileCopyrightText: 2026 Colin Phipps <cph@moria.org.uk>
 *
 * SPDX-License-Identifier: Artistic-2.0
 */

// AI: copilot / Claude Haiku

import (
	"fmt"
	"os"
	"os/exec"
	"path/filepath"
	"regexp"
	"slices"
	"strings"
	"testing"
	"time"

	zstesting "github.com/cph6/zsync/internal/testing"
)

var (
	testDataDir = "../data"
	binaryDir   = "../.."
	scratchDir  string

	proxyURL string

	targetFile = "target.dat"

	// Regex for parsing zsync output
	fetchedRegex = regexp.MustCompile(`used (\d+) local, fetched (\d+)`)
)

// TestMain sets up and tears down the test environment
func TestMain(m *testing.M) {
	setup()
	code := m.Run()
	teardown()
	os.Exit(code)
}

func setup() {
	// Get local machine IP address (excluding localhost)
	// This is needed for proxy testing since Go's HTTP client doesn't proxy localhost requests
	localIP := zstesting.GetLocalIP()
	if localIP == "" {
		// Fall back to 127.0.0.1 if we can't find the IP
		localIP = "127.0.0.1"
	}
	proxyURL = fmt.Sprintf("https://%s:8082/", localIP)

	// Create scratch directory
	var err error
	if scratchDir, err = os.MkdirTemp("", "zsync-test-"); err != nil {
		panic(fmt.Sprintf("Failed to create scratch directory: %v", err))
	}
	if err := zstesting.SetupApacheServer(".."); err != nil {
		panic(fmt.Sprintf("failed to start apache: %v", err))
	}
	// The sizes and seed numbers below are arbitrary, but the the
	// corresponding generated .zsync files committed to the
	// t/data directory must be regenerated if they are changed.
	if err := zstesting.WriteTestFile(filepath.Join(testDataDir, targetFile), 281020, 0x12d0d83dc21135be); err != nil {
		panic("failed to write test data file")
	}
	if err := zstesting.WriteTestFile(filepath.Join(testDataDir, "with-auth", targetFile), 9553, 0x6a0039489f8705d6); err != nil {
		panic("failed to write test data file")
	}
}

func teardown() {
	zstesting.TeardownApacheServer()
	// Clean up scratch directory
	if strings.HasPrefix(filepath.Base(scratchDir), "zsync-test-") {
		os.RemoveAll(scratchDir)
	}
}

// makeZSync creates a zsync file using the zsyncmake command
func makeZSync(t *testing.T, filename string, params ...string) string {
	outfile := filepath.Join(testDataDir, "test.zsync")

	args := []string{"-o", outfile, "-u", filepath.Base(filename)}
	args = append(args, params...)
	args = append(args, filename)

	cmd := exec.Command(filepath.Join(binaryDir, "zsyncmake"), args...)

	output, err := cmd.CombinedOutput()
	if err != nil {
		t.Fatalf("zsyncmake failed: %v\nOutput: %s", err, output)
	}

	return outfile
}

type tryZSyncOptions struct {
	Parameters []string
	URL        string
	Env        map[string]string
	Binary     string
}

// tryZSync runs zsync command and returns output file, stats, and stderr
func tryZSync(t *testing.T, zsyncFile string, opts tryZSyncOptions) (string, map[string]int, string, error) {
	url := opts.URL
	if url == "" {
		url = zstesting.HttpsURL
	}
	binary := filepath.Join(binaryDir, "zsync")
	if opts.Binary != "" {
		binary = opts.Binary
	}

	args := []string{}

	// Add certificate check parameter if not already specified
	hasCertParam := false
	if opts.Parameters != nil {
		for _, p := range opts.Parameters {
			if strings.HasPrefix(p, "--no-check-certificate") {
				hasCertParam = true
				break
			}
		}
	}
	if !hasCertParam && opts.Binary == "" {
		args = append(args, "--no-check-certificate")
	}

	var outfile string
	if !slices.Contains(opts.Parameters, "-o") {
		outfile = fmt.Sprintf("output-%d", time.Now().UnixNano())
		// zsync-0.6 has problems with temporary files and named output paths.
		if opts.Binary == "" {
			outfile = filepath.Join(scratchDir, outfile)
		}
		args = append(args, "-o", outfile)
	}

	if opts.Parameters != nil {
		args = append(args, opts.Parameters...)
	}
	args = append(args, url+zsyncFile)

	cmd := exec.Command(binary, args...)

	if opts.Env != nil {
		cmd.Env = os.Environ()
		for k, v := range opts.Env {
			cmd.Env = append(cmd.Env, fmt.Sprintf("%s=%s", k, v))
		}
	}

	output, err := cmd.CombinedOutput()
	stderr := string(output)
	t.Log("zsync output: ", stderr)

	// Parse transfer statistics
	stats := make(map[string]int)
	matches := fetchedRegex.FindStringSubmatch(stderr)
	if matches != nil {
		var local, fetched int
		fmt.Sscanf(matches[1], "%d", &local)
		fmt.Sscanf(matches[2], "%d", &fetched)
		stats["local"] = local
		stats["fetched"] = fetched
	}

	return outfile, stats, stderr, err
}

// TestZSyncmakeAndZsync tests the whole system - making a zsync file and then
// using it to reconstruct a target file.
func TestZSyncmakeAndZsync(t *testing.T) {
	if testing.Short() {
		t.Skip("Skipping integration test in short mode")
	}

	targetPath := filepath.Join(testDataDir, targetFile)
	controlFile := makeZSync(t, targetPath)
	defer os.Remove(controlFile)
	fmt.Fprintf(os.Stderr, "control file: %s\n", controlFile)

	seedFile := zstesting.ProvideSeed(t, scratchDir, filepath.Join(testDataDir, targetFile), 281020)
	defer os.Remove(seedFile)

	outfile1, _, _, _ := tryZSync(t, "test.zsync", tryZSyncOptions{
		Parameters: []string{"-i", seedFile},
	})
	defer os.Remove(outfile1)

	zstesting.AssertFilesEqual(t, outfile1, filepath.Join(testDataDir, targetFile))
}

// TestZSyncCaching tests zsync caching functionality
func TestZSyncCaching(t *testing.T) {
	if testing.Short() {
		t.Skip("Skipping integration test in short mode")
	}

	seedFile := filepath.Join(scratchDir, "seed-cache")
	if err := zstesting.CopyFile(filepath.Join(testDataDir, targetFile), seedFile); err != nil {
		t.Fatalf("Failed to copy seed file: %v", err)
	}

	controlFile := filepath.Join(scratchDir, "control-cache")

	opts := tryZSyncOptions{
		Parameters: []string{"-i", seedFile, "-k", controlFile},
	}
	// First run
	outfile1, _, _, _ := tryZSync(t, targetFile+".zsync", opts)

	// Second run (should use cache)
	outfile2, _, stderr, _ := tryZSync(t, targetFile+".zsync", opts)
	defer os.Remove(outfile1)
	defer os.Remove(outfile2)
	defer os.Remove(controlFile)

	if !strings.Contains(stderr, "using local copy") {
		t.Errorf("Expected 'using local copy' in stderr")
	}

	zstesting.AssertFilesEqual(t, outfile2, filepath.Join(testDataDir, targetFile))
}

// TestZSyncWithAuth tests zsync with authentication
func TestZSyncWithAuth(t *testing.T) {
	if testing.Short() {
		t.Skip("Skipping integration test in short mode")
	}

	seedFile := zstesting.ProvideSeed(t, scratchDir, filepath.Join(testDataDir, "with-auth", targetFile), 9475)
	defer os.Remove(seedFile)

	outfile, _, _, _ := tryZSync(t, filepath.Join("with-auth", targetFile+".zsync"), tryZSyncOptions{
		Parameters: []string{"-i", seedFile, "-A", "localhost=user:mypass"},
	})
	defer os.Remove(outfile)

	zstesting.AssertFilesEqual(t, outfile, filepath.Join(testDataDir, "with-auth", targetFile))
}

// TestZSyncMoreThan4G tests zsync with files larger than 4GB
func TestZSyncMoreThan4G(t *testing.T) {
	if testing.Short() || os.Getenv("LARGE_TESTS") != "yes" {
		t.Skip("Skipping integration test in short mode")
	}

	// Create sparse file
	target := filepath.Join(testDataDir, "hugefile")
	defer os.Remove(target)
	createHugeFile(t, target)

	// Create zsync file for this one on the fly, since it is
	// a bit large to want to ship it.
	zsyncFile := filepath.Join(testDataDir, "hugefile.zsync")
	cmd := exec.Command(filepath.Join(binaryDir, "zsyncmake"), "hugefile")
	cmd.Dir = testDataDir
	output, err := cmd.CombinedOutput()
	if err != nil {
		t.Fatalf("zsyncmake failed: %v\nOutput: %s", err, output)
	}
	defer os.Remove(zsyncFile)

	seedFile := filepath.Join(scratchDir, "seed-huge")
	createHugeFileSeed(t, seedFile)
	defer os.Remove(seedFile)

	start := time.Now()
	outfile, _, _, _ := tryZSync(t, "hugefile.zsync", tryZSyncOptions{
		Parameters: []string{"-i", seedFile},
	})
	elapsed := time.Since(start)
	defer os.Remove(outfile)

	md5hash, err := zstesting.HexMD5(outfile)
	if err != nil {
		t.Fatalf("Failed to calculate MD5: %v", err)
	}

	expectedHash := "1fef546312347ad0c891073cf297b7d4"
	if md5hash != expectedHash {
		t.Errorf("Expected MD5 %s, got %s", expectedHash, md5hash)
	}

	if elapsed > 1000*time.Second {
		t.Errorf("Test took too long: %v", elapsed)
	}

	t.Logf("Test completed in %d seconds", int(elapsed.Seconds()))
}

// createHugeFile creates a sparse test file
func createHugeFile(t *testing.T, path string) {
	file, err := os.Create(path)
	if err != nil {
		t.Fatalf("Failed to create huge file: %v", err)
	}
	defer file.Close()

	// Create sparse file by writing at different offsets
	data := []byte(strings.Repeat("xyzz", 1024)) // 4KB

	offsets := []int64{0, 12800 * 4096, 38400 * 4096, 384000 * 4096, 1152000 * 4096}
	for _, offset := range offsets {
		file.Seek(offset, 0)
		file.Write(data)
	}
}

// createHugeFileSeed creates a seed file for the huge file test
func createHugeFileSeed(t *testing.T, path string) {
	file, err := os.Create(path)
	if err != nil {
		t.Fatalf("Failed to create seed file: %v", err)
	}
	defer file.Close()

	// Write 4KB of "xyzzy"
	seed := []byte(strings.Repeat("xyzzy", 819))
	file.Write(seed[:4096])
}

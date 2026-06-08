package main

/*
 * SPDX-FileCopyrightText: 2026 Colin Phipps <cph@moria.org.uk>
 *
 * SPDX-License-Identifier: Artistic-2.0
 */

// AI: copilot / Claude Haiku

import (
	"fmt"
	"io"
	"net"
	"os"
	"os/exec"
	"path/filepath"
	"regexp"
	"strings"
	"testing"
	"time"
)

var (
	testDataDir = "../data"
	binaryDir   = "../.."
	scratchDir  string

	// URLs for testing
	httpURL  = "http://localhost:8081/"
	httpsURL = "https://localhost:8082/"
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
	localIP := getLocalIP()
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
	if err := SetupApacheServer(); err != nil {
		panic(fmt.Sprintf("failed to start apache: %v", err))
	}
	// The sizes and seed numbers below are arbitrary, but the the
	// corresponding generated .zsync files committed to the
	// t/data directory must be regenerated if they are changed.
	if err := writeTestFile(filepath.Join(testDataDir, targetFile), 281020, 0x12d0d83dc21135be); err != nil {
		panic("failed to write test data file")
	}
	if err := writeTestFile(filepath.Join(testDataDir, "with-auth", targetFile), 9553, 0x6a0039489f8705d6); err != nil {
		panic("failed to write test data file")
	}
}

// getLocalIP returns the local machine's IP address (excluding localhost)
// It tries to connect to a remote address to determine the local interface
func getLocalIP() string {
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

func teardown() {
	TeardownApacheServer()
	// Clean up scratch directory
	if strings.HasPrefix(filepath.Base(scratchDir), "zsync-test-") {
		os.RemoveAll(scratchDir)
	}
}

// tryZSync runs zsync command and returns output file, stats, and stderr
func tryZSync(t *testing.T, zsyncFile string, parameters []string, url string, env map[string]string) (string, map[string]int, string, error) {
	outfile := filepath.Join(scratchDir, fmt.Sprintf("output-%d", time.Now().UnixNano()))

	if url == "" {
		url = httpsURL
	}

	args := []string{}

	// Add certificate check parameter if not already specified
	hasCertParam := false
	for _, p := range parameters {
		if strings.HasPrefix(p, "--no-check-certificate") {
			hasCertParam = true
			break
		}
	}
	if !hasCertParam {
		args = append(args, "--no-check-certificate")
	}

	args = append(args, parameters...)
	args = append(args, "-o", outfile, url+zsyncFile)

	cmd := exec.Command(filepath.Join(binaryDir, "zsync"), args...)

	if env != nil {
		cmd.Env = os.Environ()
		for k, v := range env {
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

// provideSeed creates a partial seed file from source
func provideSeed(t *testing.T, source string, length int) string {
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

// TestZSyncSimpleNoLocal tests zsync without local file
func TestZSyncSimpleNoLocal(t *testing.T) {
	if testing.Short() {
		t.Skip("Skipping integration test in short mode")
	}

	outfile, stats, _, err := tryZSync(t, targetFile+".zsync", []string{}, "", nil)
	defer os.Remove(outfile)

	if err != nil {
		t.Logf("zsync output: %v", err)
	}

	assertFilesEqual(t, outfile, filepath.Join(testDataDir, targetFile))

	if stats["local"] != 0 {
		t.Errorf("Expected local=0, got %d", stats["local"])
	}
}

// TestZSyncSimpleAllLocal tests zsync with complete local seed file
func TestZSyncSimpleAllLocal(t *testing.T) {
	if testing.Short() {
		t.Skip("Skipping integration test in short mode")
	}

	// Create seed file
	seedFile := filepath.Join(scratchDir, "seed")
	if err := copyFile(filepath.Join(testDataDir, targetFile), seedFile); err != nil {
		t.Fatalf("Failed to copy seed file: %v", err)
	}

	outfile, stats, _, _ := tryZSync(t, targetFile+".zsync", []string{"-i", seedFile}, "", nil)
	defer os.Remove(outfile)

	assertFilesEqual(t, outfile, filepath.Join(testDataDir, targetFile))

	if stats["fetched"] != 0 {
		t.Errorf("Expected fetched=0, got %d", stats["fetched"])
	}

	// Check that backup file was not created
	backupFile := outfile + ".zs-old"
	if _, err := os.Stat(backupFile); err == nil {
		t.Error("Backup file should not have been created")
		os.Remove(backupFile)
	}
}

// TestZSyncSimpleSomeLocal tests zsync with partial local seed file
func TestZSyncSimpleSomeLocal(t *testing.T) {
	if testing.Short() {
		t.Skip("Skipping integration test in short mode")
	}

	seedFile := provideSeed(t, filepath.Join(testDataDir, targetFile), 281020)
	defer os.Remove(seedFile)

	outfile, stats, _, _ := tryZSync(t, targetFile+".zsync", []string{"-i", seedFile}, "", nil)
	defer os.Remove(outfile)

	assertFilesEqual(t, outfile, filepath.Join(testDataDir, targetFile))

	if stats["local"] <= 0 {
		t.Errorf("Expected local > 0, got %d", stats["local"])
	}
	if stats["fetched"] <= 0 {
		t.Errorf("Expected fetched > 0, got %d", stats["fetched"])
	}
}

// TestZSyncMatchContinuation tests zsync on a control file with a shorter
// blocksize and match continuation.
func TestZSyncMatchContinuation(t *testing.T) {
	if testing.Short() {
		t.Skip("Skipping integration test in short mode")
	}

	seedFile := provideSeed(t, filepath.Join(testDataDir, targetFile), 281020)
	defer os.Remove(seedFile)

	outfile, stats, _, _ := tryZSync(t, targetFile+".sm2.zsync", []string{"-i", seedFile}, "", nil)
	defer os.Remove(outfile)

	assertFilesEqual(t, outfile, filepath.Join(testDataDir, targetFile))

	if stats["local"] <= 0 {
		t.Errorf("Expected local > 0, got %d", stats["local"])
	}
	if stats["fetched"] <= 0 {
		t.Errorf("Expected fetched > 0, got %d", stats["fetched"])
	}
}

// TestZSyncCaching tests zsync caching functionality
func TestZSyncCaching(t *testing.T) {
	if testing.Short() {
		t.Skip("Skipping integration test in short mode")
	}

	seedFile := filepath.Join(scratchDir, "seed-cache")
	if err := copyFile(filepath.Join(testDataDir, targetFile), seedFile); err != nil {
		t.Fatalf("Failed to copy seed file: %v", err)
	}

	controlFile := filepath.Join(scratchDir, "control-cache")

	// First run
	outfile1, _, _, _ := tryZSync(t, targetFile+".zsync", []string{"-i", seedFile, "-k", controlFile}, "", nil)

	// Second run (should use cache)
	outfile2, _, stderr, _ := tryZSync(t, targetFile+".zsync", []string{"-i", seedFile, "-k", controlFile}, "", nil)
	defer os.Remove(outfile1)
	defer os.Remove(outfile2)
	defer os.Remove(controlFile)

	if !strings.Contains(stderr, "using local copy") {
		t.Errorf("Expected 'using local copy' in stderr")
	}

	assertFilesEqual(t, outfile2, filepath.Join(testDataDir, targetFile))
}

// TestZSyncSimpleSomeLocal tests zsync with partial local seed file
func TestZSyncProxy(t *testing.T) {
	if testing.Short() {
		t.Skip("Skipping integration test in short mode")
	}

	SetupProxyServer(t)
	defer TeardownProxyServer()

	seedFile := provideSeed(t, filepath.Join(testDataDir, targetFile), 281020)
	defer os.Remove(seedFile)

	outfile, stats, _, _ := tryZSync(t, targetFile+".zsync", []string{"-i", seedFile}, proxyURL, map[string]string{"https_proxy": "http://localhost:8083/"})
	defer os.Remove(outfile)

	assertFilesEqual(t, outfile, filepath.Join(testDataDir, targetFile))

	if stats["local"] <= 0 {
		t.Errorf("Expected local > 0, got %d", stats["local"])
	}
	if stats["fetched"] <= 0 {
		t.Errorf("Expected fetched > 0, got %d", stats["fetched"])
	}
}

// TestZSyncBadSSL tests zsync with invalid SSL certificate
func TestZSyncBadSSL(t *testing.T) {
	if testing.Short() {
		t.Skip("Skipping integration test in short mode")
	}

	outfile, _, stderr, _ := tryZSync(t, targetFile+".zsync", []string{"--no-check-certificate=false"}, httpsURL, nil)
	defer os.Remove(outfile)

	if !strings.Contains(stderr, "certificate is not valid") && !strings.Contains(stderr, "failed to verify certificate") {
		t.Logf("stderr: %s", stderr)
		t.Errorf("Expected certificate validation error")
	}
}

// TestZSyncNoSSL tests zsync over HTTP (no SSL)
func TestZSyncNoSSL(t *testing.T) {
	if testing.Short() {
		t.Skip("Skipping integration test in short mode")
	}

	seedFile := provideSeed(t, filepath.Join(testDataDir, targetFile), 281020)
	defer os.Remove(seedFile)

	outfile, stats, _, _ := tryZSync(t, targetFile+".zsync", []string{"-i", seedFile}, httpURL, nil)
	defer os.Remove(outfile)

	assertFilesEqual(t, outfile, filepath.Join(testDataDir, targetFile))

	if stats["local"] <= 0 {
		t.Errorf("Expected local > 0, got %d", stats["local"])
	}
	if stats["fetched"] <= 0 {
		t.Errorf("Expected fetched > 0, got %d", stats["fetched"])
	}
}

// TestZSyncWithAuth tests zsync with authentication
func TestZSyncWithAuth(t *testing.T) {
	if testing.Short() {
		t.Skip("Skipping integration test in short mode")
	}

	seedFile := provideSeed(t, filepath.Join(testDataDir, "with-auth", targetFile), 9475)
	defer os.Remove(seedFile)

	outfile, _, _, _ := tryZSync(t, filepath.Join("with-auth", targetFile+".zsync"), []string{
		"-i", seedFile, "-A", "localhost=user:mypass",
	}, "", nil)
	defer os.Remove(outfile)

	assertFilesEqual(t, outfile, filepath.Join(testDataDir, "with-auth", targetFile))
}

// TestZSyncRedirect tests zsync with redirected zsync file
func TestZSyncRedirect(t *testing.T) {
	if testing.Short() {
		t.Skip("Skipping integration test in short mode")
	}

	seedFile := provideSeed(t, filepath.Join(testDataDir, targetFile), 281020)
	defer os.Remove(seedFile)

	outfile, stats, _, _ := tryZSync(t, targetFile+"2.zsync", []string{"-i", seedFile}, "", nil)
	defer os.Remove(outfile)

	assertFilesEqual(t, outfile, filepath.Join(testDataDir, targetFile))

	if stats["local"] <= 0 {
		t.Errorf("Expected local > 0, got %d", stats["local"])
	}
	if stats["fetched"] <= 0 {
		t.Errorf("Expected fetched > 0, got %d", stats["fetched"])
	}
}

// TestZSyncTargetRedirect tests zsync with redirected target file
func TestZSyncTargetRedirect(t *testing.T) {
	if testing.Short() {
		t.Skip("Skipping integration test in short mode")
	}

	seedFile := provideSeed(t, filepath.Join(testDataDir, targetFile), 281020)
	defer os.Remove(seedFile)

	outfile, stats, _, _ := tryZSync(t, targetFile+"3.zsync", []string{"-i", seedFile}, "", nil)
	defer os.Remove(outfile)

	assertFilesEqual(t, outfile, filepath.Join(testDataDir, targetFile))

	if stats["local"] <= 0 {
		t.Errorf("Expected local > 0, got %d", stats["local"])
	}
	if stats["fetched"] <= 0 {
		t.Errorf("Expected fetched > 0, got %d", stats["fetched"])
	}
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
	outfile, _, _, _ := tryZSync(t, "hugefile.zsync", []string{"-i", seedFile}, "", nil)
	elapsed := time.Since(start)
	defer os.Remove(outfile)

	md5hash, err := hexMD5(outfile)
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

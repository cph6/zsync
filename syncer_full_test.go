package zsync

/*
 * SPDX-FileCopyrightText: 2026 Colin Phipps <cph@moria.org.uk>
 *
 * SPDX-License-Identifier: Artistic-2.0
 */

// AI: opencode / glm-5.2

// These tests exercise the zsync library (Syncer) end-to-end against the same
// Apache / tinyproxy / TLS server setup that the CLI integration tests in
// integration_test.go use, but in-process: instead of shelling out to the
// zsync binary they construct a *Syncer, feed it seed data, and call
// FetchRemainingBlocks directly.

import (
	"crypto/tls"
	"fmt"
	"net/http"
	"os"
	"path/filepath"
	"regexp"
	"strings"
	"testing"
	"time"

	"github.com/cph6/zsync/internal/httpbasic"
	zstesting "github.com/cph6/zsync/internal/testing"
)

// TestMain sets up and tears down the test environment
func TestMain(m *testing.M) {
	setup()
	code := m.Run()
	teardown()
	os.Exit(code)
}

var (
	proxyURL    string
	scratchDir  string
	testDataDir = "t/data"
	targetFile  = "target.dat"

	// Regex for parsing zsync output
	fetchedRegex = regexp.MustCompile(`used (\d+) local, fetched (\d+)`)
)

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
	if err := zstesting.SetupApacheServer("t"); err != nil {
		panic(fmt.Sprintf("failed to start apache: %v", err))
	}
	// The sizes and seed numbers below are arbitrary, but the the
	// corresponding generated .zsync files committed to the
	// t/data directory must be regenerated if they are changed.
	if err := zstesting.WriteTestFile(filepath.Join(testDataDir, targetFile), 281020, 0x12d0d83dc21135be); err != nil {
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

// syncConfig configures a library-based sync run.
type syncConfig struct {
	// baseURL is the URL prefix from which the .zsync control file is fetched.
	// Defaults to HttpsURL when empty.
	baseURL string
	// seedFiles are local files to feed to the Syncer as seed data.
	seedFiles []string
	// skipVerify disables TLS certificate verification (the test server uses a
	// self-signed cert).
	skipVerify bool
	// referer is an explicit referer URL for relative-URL resolution; normally
	// the Syncer uses the URL the control file was fetched from.
	referer string
	// outputFilename overrides the reconstructed target filename (in scratchDir).
	// When empty, runSyncer generates a unique name so concurrent runs don't
	// collide.
	outputFilename string
}

// syncResult captures the outcome of running a Syncer end-to-end.
type syncResult struct {
	// finalFile is the path of the file the syncer left behind: the reconstructed
	// target on success, or the .part temp file on failure.
	finalFile string
	// localBytes is the number of bytes the Syncer obtained from seed data.
	localBytes int64
	// fetchedBytes is the number of bytes the Syncer downloaded over HTTP.
	fetchedBytes int64
	// err is the first error encountered (control fetch, seed read, sync, or
	// finalize). It is nil only when the full reconstruction + verification
	// succeeded.
	err error
}

// runSyncer mirrors what the zsync CLI does for a single invocation, but
// in-process: fetch the control file over HTTP, build a Syncer, feed it any
// local seed data, rename its temp file to a .part file (as the CLI does, to
// allow for resume-on-interruption), download the remaining blocks, and
// finalize the result when the syncer reports CompleteData.
func runSyncer(t *testing.T, zsyncFile string, cfg syncConfig) syncResult {
	t.Helper()

	baseURL := cfg.baseURL
	if baseURL == "" {
		baseURL = zstesting.HttpsURL
	}

	client := &httpbasic.Client{
		Client: http.Client{
			Transport: &http.Transport{
				ForceAttemptHTTP2: true,
				Proxy:             http.ProxyFromEnvironment,
				TLSClientConfig:   &tls.Config{InsecureSkipVerify: cfg.skipVerify},
			},
		},
		UserAgent: "zsync-test",
	}

	source := baseURL + zsyncFile
	controlReader, referer, err := GetControlFile(client, source, "", cfg.referer)
	if err != nil {
		return syncResult{err: err}
	}
	defer controlReader.Close()

	outName := cfg.outputFilename
	if outName == "" {
		outName = fmt.Sprintf("libout-%d", time.Now().UnixNano())
	}
	zs, err := NewFromControlFile(controlReader, SyncerOptions{
		TargetDirectory:        scratchDir,
		OverrideTargetFilename: outName,
	})
	if err != nil {
		return syncResult{err: fmt.Errorf("parse control file: %w", err)}
	}

	// Mirror the CLI behaviour of relocating the temp file to a .part file
	// during reconstruction.
	tempFile := zs.GetTargetFilename() + ".part"
	if err := zs.RenameFile(tempFile); err != nil {
		return syncResult{err: fmt.Errorf("rename failed: %w", err), finalFile: tempFile}
	}

	for _, seed := range cfg.seedFiles {
		if zs.Status() == CompleteData {
			break
		}
		f, err := os.Open(seed)
		if err != nil {
			return syncResult{err: fmt.Errorf("open seed %s: %w", seed, err), finalFile: tempFile}
		}
		if _, err := zs.NewSeedSink(nil).ReadFrom(f); err != nil {
			f.Close()
			return syncResult{err: fmt.Errorf("read seed %s: %w", seed, err), finalFile: tempFile}
		}
		f.Close()
	}

	localGot, _ := zs.Progress()

	fetched, fetchErr := zs.FetchRemainingBlocks(client, referer, nil)
	res := syncResult{
		localBytes:   localGot,
		fetchedBytes: fetched,
		finalFile:    tempFile,
	}
	if fetchErr != nil || zs.Status() != CompleteData {
		res.err = fetchErr
		return res
	}

	fname, finalErr := zs.Finalize()
	res.finalFile = fname
	res.err = finalErr
	return res
}

// assertSyncStats asserts local>0 and fetched>0 — the contract used by tests
// that supply a partial seed and expect to both reuse some local data and
// download the remainder.
func assertSyncStats(t *testing.T, r syncResult) {
	t.Helper()
	if r.localBytes <= 0 {
		t.Errorf("expected local > 0, got %d", r.localBytes)
	}
	if r.fetchedBytes <= 0 {
		t.Errorf("expected fetched > 0, got %d", r.fetchedBytes)
	}
}

// TestSyncerSimpleNoLocal tests downloading a complete target with no local
// seed data.
func TestSyncerSimpleNoLocal(t *testing.T) {
	if testing.Short() {
		t.Skip("Skipping integration test in short mode")
	}

	res := runSyncer(t, targetFile+".zsync", syncConfig{skipVerify: true})
	defer os.Remove(res.finalFile)

	if res.err != nil {
		t.Fatalf("sync failed: %v", res.err)
	}
	zstesting.AssertFilesEqual(t, res.finalFile, filepath.Join(testDataDir, targetFile))
	if res.localBytes != 0 {
		t.Errorf("expected local=0, got %d", res.localBytes)
	}
}

// TestSyncerSimpleAllLocal tests syncing a target when the complete target is
// already available locally as a seed file.
func TestSyncerSimpleAllLocal(t *testing.T) {
	if testing.Short() {
		t.Skip("Skipping integration test in short mode")
	}

	seedFile := filepath.Join(scratchDir, "seed")
	if err := zstesting.CopyFile(filepath.Join(testDataDir, targetFile), seedFile); err != nil {
		t.Fatalf("Failed to copy seed file: %v", err)
	}
	defer os.Remove(seedFile)

	res := runSyncer(t, targetFile+".zsync", syncConfig{skipVerify: true, seedFiles: []string{seedFile}})
	defer os.Remove(res.finalFile)

	if res.err != nil {
		t.Fatalf("sync failed: %v", res.err)
	}
	zstesting.AssertFilesEqual(t, res.finalFile, filepath.Join(testDataDir, targetFile))
	if res.fetchedBytes != 0 {
		t.Errorf("expected fetched=0, got %d", res.fetchedBytes)
	}
}

// TestSyncerSimpleSomeLocal tests syncing with a partial local seed file:
// the syncer should reuse some local data and download the rest.
func TestSyncerSimpleSomeLocal(t *testing.T) {
	if testing.Short() {
		t.Skip("Skipping integration test in short mode")
	}

	seedFile := zstesting.ProvideSeed(t, scratchDir, filepath.Join(testDataDir, targetFile), 281020)
	defer os.Remove(seedFile)

	res := runSyncer(t, targetFile+".zsync", syncConfig{skipVerify: true, seedFiles: []string{seedFile}})
	defer os.Remove(res.finalFile)

	if res.err != nil {
		t.Fatalf("sync failed: %v", res.err)
	}
	zstesting.AssertFilesEqual(t, res.finalFile, filepath.Join(testDataDir, targetFile))
	assertSyncStats(t, res)
}

// TestSyncerSimpleMD5 tests syncing a control file that uses MD5 as the strong
// hash algorithm.
func TestSyncerSimpleMD5(t *testing.T) {
	if testing.Short() {
		t.Skip("Skipping integration test in short mode")
	}

	seedFile := zstesting.ProvideSeed(t, scratchDir, filepath.Join(testDataDir, targetFile), 281020)
	defer os.Remove(seedFile)

	res := runSyncer(t, targetFile+".md5.zsync", syncConfig{skipVerify: true, seedFiles: []string{seedFile}})
	defer os.Remove(res.finalFile)

	if res.err != nil {
		t.Fatalf("sync failed: %v", res.err)
	}
	zstesting.AssertFilesEqual(t, res.finalFile, filepath.Join(testDataDir, targetFile))
	assertSyncStats(t, res)
}

// TestSyncerSimpleSHA224 tests syncing a control file that uses SHA-224 as the
// strong hash algorithm.
func TestSyncerSimpleSHA224(t *testing.T) {
	if testing.Short() {
		t.Skip("Skipping integration test in short mode")
	}

	seedFile := zstesting.ProvideSeed(t, scratchDir, filepath.Join(testDataDir, targetFile), 281020)
	defer os.Remove(seedFile)

	res := runSyncer(t, targetFile+".sha224.zsync", syncConfig{skipVerify: true, seedFiles: []string{seedFile}})
	defer os.Remove(res.finalFile)

	if res.err != nil {
		t.Fatalf("sync failed: %v", res.err)
	}
	zstesting.AssertFilesEqual(t, res.finalFile, filepath.Join(testDataDir, targetFile))
	assertSyncStats(t, res)
}

// TestSyncerMatchContinuation tests a control file with a shorter blocksize
// and seqMatches=2 (match continuation).
func TestSyncerMatchContinuation(t *testing.T) {
	if testing.Short() {
		t.Skip("Skipping integration test in short mode")
	}

	seedFile := zstesting.ProvideSeed(t, scratchDir, filepath.Join(testDataDir, targetFile), 281020)
	defer os.Remove(seedFile)

	res := runSyncer(t, targetFile+".sm2.zsync", syncConfig{skipVerify: true, seedFiles: []string{seedFile}})
	defer os.Remove(res.finalFile)

	if res.err != nil {
		t.Fatalf("sync failed: %v", res.err)
	}
	zstesting.AssertFilesEqual(t, res.finalFile, filepath.Join(testDataDir, targetFile))
	assertSyncStats(t, res)
}

// TestSyncerBadTargetData tests fetching a target whose remote content does
// not match the block checksums in the control file. The syncer should fail,
// and a pre-existing copy of the target file must be left untouched.
func TestSyncerBadTargetData(t *testing.T) {
	if testing.Short() {
		t.Skip("Skipping integration test in short mode")
	}

	existing := filepath.Join(scratchDir, "bad-target-data")
	if err := os.WriteFile(existing, []byte("abcd"), 0644); err != nil {
		t.Fatalf("failed to create pre-existing file: %v", err)
	}
	defer os.Remove(existing)

	res := runSyncer(t, targetFile+".bad.zsync", syncConfig{
		skipVerify:     true,
		outputFilename: "bad-target-data",
	})
	defer os.Remove(res.finalFile)

	if res.err == nil {
		t.Errorf("expected sync to fail, but it succeeded")
	}

	info, err := os.Stat(existing)
	if err != nil {
		t.Errorf("previous version of output deleted by syncer? %v", err)
	} else if info.Size() != 4 {
		t.Errorf("previous version of output has been overwritten? size=%d", info.Size())
	}
}

// TestSyncerBadChecksum tests fetching a target whose block checksums match but
// whose declared file-hash is wrong. The syncer should reconstruct the file
// but the Syncer's verification step should reject it — and a pre-existing
// copy of the target file must not be overwritten.
func TestSyncerBadChecksum(t *testing.T) {
	if testing.Short() {
		t.Skip("Skipping integration test in short mode")
	}

	existing := filepath.Join(scratchDir, "bad-checksum")
	if err := os.WriteFile(existing, []byte("abcd"), 0644); err != nil {
		t.Fatalf("failed to create pre-existing file: %v", err)
	}
	defer os.Remove(existing)

	res := runSyncer(t, targetFile+".bad-checksum.zsync", syncConfig{
		skipVerify:     true,
		outputFilename: "bad-checksum",
	})
	defer os.Remove(res.finalFile)

	if res.err == nil {
		t.Errorf("expected sync to fail, but it succeeded")
	} else if !strings.Contains(res.err.Error(), "checksum mismatch") {
		t.Errorf("expected 'checksum mismatch' error, got: %v", res.err)
	}

	info, err := os.Stat(existing)
	if err != nil {
		t.Errorf("previous version of output deleted by syncer? %v", err)
	} else if info.Size() != 4 {
		t.Errorf("previous version of output has been overwritten? size=%d", info.Size())
	}
}

// TestSyncerBadSSL tests that the Syncer refuses to connect to a server with an
// untrusted (self-signed) certificate when InsecureSkipVerify is false.
func TestSyncerBadSSL(t *testing.T) {
	if testing.Short() {
		t.Skip("Skipping integration test in short mode")
	}

	res := runSyncer(t, targetFile+".zsync", syncConfig{skipVerify: false})
	defer os.Remove(res.finalFile)

	if res.err == nil {
		t.Errorf("expected certificate validation error")
	} else if !strings.Contains(res.err.Error(), "certificate") && !strings.Contains(res.err.Error(), "x509") {
		t.Errorf("expected certificate verification error, got: %v", res.err)
	}
}

// TestSyncerNoSSL tests syncing over plain HTTP (no TLS).
func TestSyncerNoSSL(t *testing.T) {
	if testing.Short() {
		t.Skip("Skipping integration test in short mode")
	}

	seedFile := zstesting.ProvideSeed(t, scratchDir, filepath.Join(testDataDir, targetFile), 281020)
	defer os.Remove(seedFile)

	res := runSyncer(t, targetFile+".zsync", syncConfig{baseURL: zstesting.HttpURL, seedFiles: []string{seedFile}})
	defer os.Remove(res.finalFile)

	if res.err != nil {
		t.Fatalf("sync failed: %v", res.err)
	}
	zstesting.AssertFilesEqual(t, res.finalFile, filepath.Join(testDataDir, targetFile))
	assertSyncStats(t, res)
}

// TestSyncerViaProxy tests that the Syncer can download via an HTTP proxy.
func TestSyncerViaProxy(t *testing.T) {
	if testing.Short() {
		t.Skip("Skipping integration test in short mode")
	}

	zstesting.SetupProxyServer(t, "t")
	defer zstesting.TeardownProxyServer()

	// Have the http.Transport route HTTPS requests through the local proxy.
	os.Setenv("https_proxy", "http://localhost:8083/")
	defer os.Unsetenv("https_proxy")

	seedFile := zstesting.ProvideSeed(t, scratchDir, filepath.Join(testDataDir, targetFile), 281020)
	defer os.Remove(seedFile)

	// proxyURL uses the machine's non-loopback IP, otherwise the Go HTTP
	// client bypasses the proxy entirely (per http.ProxyFromEnvironment rules).
	res := runSyncer(t, targetFile+".zsync", syncConfig{
		baseURL:    proxyURL,
		skipVerify: true,
		seedFiles:  []string{seedFile},
	})
	defer os.Remove(res.finalFile)

	if res.err != nil {
		t.Fatalf("sync failed: %v", res.err)
	}
	zstesting.AssertFilesEqual(t, res.finalFile, filepath.Join(testDataDir, targetFile))
	assertSyncStats(t, res)
}

// TestSyncerRedirect tests a control file that is served via an HTTP redirect.
func TestSyncerRedirect(t *testing.T) {
	if testing.Short() {
		t.Skip("Skipping integration test in short mode")
	}

	seedFile := zstesting.ProvideSeed(t, scratchDir, filepath.Join(testDataDir, targetFile), 281020)
	defer os.Remove(seedFile)

	res := runSyncer(t, targetFile+"2.zsync", syncConfig{skipVerify: true, seedFiles: []string{seedFile}})
	defer os.Remove(res.finalFile)

	if res.err != nil {
		t.Fatalf("sync failed: %v", res.err)
	}
	zstesting.AssertFilesEqual(t, res.finalFile, filepath.Join(testDataDir, targetFile))
	assertSyncStats(t, res)
}

// TestSyncerTargetRedirect tests a control file whose target URL is served via
// an HTTP redirect.
func TestSyncerTargetRedirect(t *testing.T) {
	if testing.Short() {
		t.Skip("Skipping integration test in short mode")
	}

	seedFile := zstesting.ProvideSeed(t, scratchDir, filepath.Join(testDataDir, targetFile), 281020)
	defer os.Remove(seedFile)

	res := runSyncer(t, targetFile+"3.zsync", syncConfig{skipVerify: true, seedFiles: []string{seedFile}})
	defer os.Remove(res.finalFile)

	if res.err != nil {
		t.Fatalf("sync failed: %v", res.err)
	}
	zstesting.AssertFilesEqual(t, res.finalFile, filepath.Join(testDataDir, targetFile))
	assertSyncStats(t, res)
}

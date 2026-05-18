package main

/*
 * SPDX-FileCopyrightText: 2004,2005,2007,2009,2025,2026 Colin Phipps <cph@moria.org.uk>
 *
 * SPDX-License-Identifier: Artistic-2.0
 */

// AI: copilot / Raptor mini conversion of zsync's main.c, albeit with
// considerable simplifcation compared to the original client: no longer
// putting multiple ranges in one request, no pipelining.

import (
	"bufio"
	"context"
	"crypto/tls"
	"errors"
	"flag"
	"fmt"
	"io"
	"math/rand"
	"net/http"
	"net/url"
	"os"
	"path"
	"path/filepath"
	"strings"
	"sync"
	"time"
	"unicode"

	"github.com/cph6/zsync/internal/zsync"
	"golang.org/x/sync/errgroup"
)

const version = "0.7.0"

// stringSlice for repeatable flag values.
type stringSlice []string

func (s *stringSlice) String() string {
	return strings.Join(*s, ",")
}

func (s *stringSlice) Set(value string) error {
	*s = append(*s, value)
	return nil
}

type authCred struct {
	username string
	password string
}

type authMap map[string]authCred

func (a authMap) String() string {
	entries := make([]string, 0, len(a))
	for host, cred := range a {
		entries = append(entries, fmt.Sprintf("%s=%s:%s", host, cred.username, cred.password))
	}
	return strings.Join(entries, ",")
}

func (a authMap) Set(value string) error {
	parts := strings.SplitN(value, "=", 2)
	if len(parts) != 2 {
		return fmt.Errorf("invalid auth syntax: %s", value)
	}
	host := parts[0]
	pair := parts[1]
	creds := strings.SplitN(pair, ":", 2)
	if len(creds) != 2 {
		return fmt.Errorf("invalid auth syntax: %s", value)
	}
	a[host] = authCred{username: creds[0], password: creds[1]}
	return nil
}

// Returns a suggested filename for the target file, given the source path of
// the zsync control file.
func getFilenameBase(source string) string {
	name := source
	if u, err := url.Parse(source); err == nil && u.Scheme != "" && u.Host != "" {
		name = path.Base(u.Path)
	} else {
		name = filepath.Base(source)
	}

	// Usually the control file is the filename + .zsync, so stripping that gives
	// a good guess at the target filename.
	return strings.TrimSuffix(name, ".zsync")
}

// Returns a textual prefix of the filename part of the source filename.
func getFilenamePrefix(source string) string {
	name := getFilenameBase(source)
	for i, r := range name {
		if !(unicode.IsLetter(r) || unicode.IsNumber(r)) {
			return name[:i]
		}
	}
	return name
}

func getFilename(zs *zsync.State, source string) string {
	// First try using the filename specified in the zsync control file.
	name := zs.Filename()
	if name != "" {
		// 1. Strip any path component from filename supplied by the remote.
		//    i.e. the remote side should not be able to ask for ../../something to
		//    be targetted for writing.
		name := filepath.Base(name)
		// 2. Accept the name only if it has a common prefix with the name of the
		//    zsync control file. This is a principle of least surprise check:
		//    if the user ran `zsync https://debian.org/debian-15.0.iso.zsync`, the
		//    target file written should not be `mbox`.
		prefix := getFilenamePrefix(source)
		if prefix != "" && strings.HasPrefix(name, prefix) {
			return name
		}
		if prefix != "" {
			fmt.Fprintf(os.Stderr, "Rejected filename specified in %s - prefix %s differed from filename %s.\n", source, prefix, name)
		}
	}
	// Fallback to using the filename part of the source URL or path. Since that
	// is what the user gave us, that name should never surprise them.
	prefix := getFilenameBase(source)
	if prefix != "" {
		return prefix
	}
	// If the user asked for a URL with no filename component and somehow gets
	// back a valid zsync file, then fallback to a hardcoded name.
	return "zsync-download"
}

// Checks for a bad filename supplied for writing the local copy of the control
// file.
func checkSuppliedFilename(filename string) error {
	// If the file ends in .zsync, assume it's fine. If the file is new, it's fine.
	if _, err := os.Stat(filename); errors.Is(err, os.ErrNotExist) {
		return nil
	}
	if strings.HasSuffix(filename, ".zsync") {
		return nil
	}
	f, err := os.Open(filename)
	if err != nil {
		return fmt.Errorf("failed to verify that %s is a zsync control file: %w", filename, err)
	}
	defer f.Close()
	reader := bufio.NewReader(f)
	line, err := reader.ReadString('\n')
	if err != nil {
		return fmt.Errorf("failed to verify that %s is a zsync control file: %w", filename, err)
	}
	if !strings.HasPrefix(line, "zsync:") {
		return fmt.Errorf("Refusing to overwrite %s with zsync control file. Did you use -k on the wrong file?", filename)
	}
	return nil
}

func main() {
	var (
		auths      authMap = make(authMap)
		seedFiles  stringSlice
		filename   string
		keepZsync  string
		quiet      bool
		verbose    bool
		referer    string
		showVer    bool
		skipVerify bool
	)

	flag.Var(&auths, "A", "hostname=username:password")
	// It is unfortunate that 20 years ago I used -k for the .zsync path,
	// otherwise I could have used it for allowing untrusted SSL connections
	// as in curl. Oh well.
	flag.StringVar(&keepZsync, "k", "", "save a copy of the .zsync file to this path. If the download is interrupted, the download can be resumed using this local copy instead of redownloading.")

	flag.StringVar(&filename, "o", "", "output filename")
	flag.Var(&seedFiles, "i", "seed file to supply as local source data")
	flag.BoolVar(&skipVerify, "no-check-certificate", false, "Disable verifying the SSL certificate of any target server. NOTE: this makes the file transfer vulnerable to man-in-the-middle attacks.")
	flag.BoolVar(&showVer, "V", false, "show version")
	flag.BoolVar(&quiet, "q", false, "suppress progress output")
	flag.StringVar(&referer, "u", "", "If a local zsync file is supplied, this supplies the URL from which the .zsync file is or could be downloaded - this is used for resolving relative URLs in the .zsync file, as if the .zsync was downloaded from this URL.")
	flag.BoolVar(&verbose, "v", false, "verbose mode - reports some debugging stats")
	flag.Parse()

	if showVer {
		fmt.Printf("zsync v%s\n", version)
		os.Exit(0)
	}

	args := flag.Args()
	if len(args) != 1 {
		fmt.Fprintf(os.Stderr, "Usage: zsync [options] <path-or-url-to-.zsync>\n")
		flag.PrintDefaults()
		os.Exit(3)
	}
	source := args[0]

	client := &http.Client{
		Transport: &http.Transport{
			ForceAttemptHTTP2: true,
			TLSClientConfig:   &tls.Config{InsecureSkipVerify: skipVerify},
		}}

	if err := checkSuppliedFilename(keepZsync); err != nil {
		fmt.Fprintf(os.Stderr, "%v", err)
		os.Exit(3)
	}
	zs, err := readZsyncControlFile(client, source, keepZsync, referer, auths)
	if err != nil {
		fmt.Fprintf(os.Stderr, "failed reading control file: %v\n", err)
		os.Exit(3)
	}

	if referer == "" {
		referer = source
	}

	if filename == "" {
		filename = getFilename(zs, source)
	}
	tempFile := filename + ".part"

	// If the target file, or a temporary file from a previous attempt, exists,
	// add them to the list of seed files to read from first. We expect that
	// these are likely to be more useful than other seed files.
	if _, err := os.Stat(filename); err == nil {
		seedFiles = append([]string{filename}, seedFiles...)
	}
	if _, err := os.Stat(tempFile); err == nil {
		seedFiles = append([]string{tempFile}, seedFiles...)
	}

	if err := zs.Prepare(filename); err != nil {
		fmt.Fprintf(os.Stderr, "failed to prepare temporary file: %v", err)
		exitWithCode(1)
	}

	localUsed := int64(0)
	for _, file := range seedFiles {
		if !quiet {
			fmt.Fprintf(os.Stderr, "reading seed file %s:", file)
		}
		if err := readSeedFile(zs, file, quiet); err != nil {
			fmt.Fprintf(os.Stderr, "%v\n", err)
			exitWithCode(1)
		}

		got, total := zs.Progress()
		localUsed = got
		if !quiet {
			fmt.Fprintf(os.Stderr, "Done reading %s. %02.1f%% of target obtained.      \n", file, float64(got)/float64(total)*100.0)
		}
		if zs.Status() >= 2 {
			break
		}
	}

	if localUsed == 0 && !quiet {
		fmt.Fprintln(os.Stderr, "No relevent local data found - I will be downloading the whole file. If that's not what you want, CTRL-C out. You should specify the local file is the old version of the file to download with -i (you might have to decompress it with gzip -d first). Or perhaps you just have no data that helps download the file")
	}

	if err := zs.RenameFile(tempFile); err != nil {
		fmt.Fprintf(os.Stderr, "rename failed: %v\n", err)
		exitWithCode(1)
	}

	fetchErr := fetchRemainingBlocks(client, zs, referer, auths, quiet)
	if fetchErr != nil || zs.Status() < 2 {
		errMsg := ""
		if fetchErr != nil {
			errMsg = fetchErr.Error()
		}
		fmt.Fprintf(os.Stderr, "Not all of the required data could be downloaded, and the remaining data could not be retrieved from any of the download URLs. %s\n", errMsg)
		fmt.Fprintf(os.Stderr, "%s. Incomplete transfer left in %s.\n", "completed download left in", tempFile)
		exitWithCode(3)
	}

	if verbose {
		s := zs.Stats()
		fmt.Printf("hash stats: bithash hit %d, weak hit %d, checksums calculated %d, strong hit %d\n", s.HashHit, s.WeakHit, s.Checksummed, s.StrongHit)
	}
	if !quiet {
		fmt.Print("verifying download...")
	}
	if err := zs.Complete(); err != nil {
		fmt.Fprintf(os.Stderr, "failed(%v), download available in %s\n", err, tempFile)
		exitWithCode(2)
	}
	if !quiet {
		fmt.Println("checksum matches OK")
	}

	tempFilename := zsync.End(zs)
	mtime := zs.Mtime()

	if filename != "" {
		oldBackup := filename + ".zs-old"
		ok := true

		if _, err := os.Stat(filename); err == nil {
			_ = os.Remove(oldBackup)
			if err := os.Link(filename, oldBackup); err != nil {
				if err2 := os.Rename(filename, oldBackup); err2 != nil {
					fmt.Fprintf(os.Stderr, "Unable to back up old file %s - completed download left in %s\n", filename, tempFilename)
					ok = false
				}
			}
		}

		if ok {
			if err := os.Rename(tempFilename, filename); err != nil {
				fmt.Fprintf(os.Stderr, "Unable to move %s to %s: %v\n", tempFilename, filename, err)
				ok = false
			}
			if ok && !mtime.IsZero() {
				if err := os.Chtimes(filename, time.Now(), mtime); err != nil {
					fmt.Fprintf(os.Stderr, "warning: failed to set mtime: %v\n", err)
				}
			}
		}
	} else {
		fmt.Printf("No filename specified for download - completed download left in %s\n", tempFilename)
	}

	if !quiet {
		fmt.Printf("used %d local, fetched %d\n", localUsed, httpBytesDownloaded)
	}
}

func exitWithCode(code int) {
	os.Exit(code)
}

func readZsyncControlFile(client *http.Client, source, keepZsync, referer string, auths authMap) (*zsync.State, error) {
	// First try to read from local file.
	if _, err := os.Stat(source); err == nil {
		f, err := os.Open(source)
		if err != nil {
			return nil, fmt.Errorf("failed to open .zsync: %w", err)
		}
		defer f.Close()
		zs, err := zsync.New(f)
		return zs, err
	}

	// Otherwise, try to download from URL.
	u, err := url.Parse(source)
	if err != nil || u.Scheme == "" {
		return nil, fmt.Errorf("%s is not a valid URL or local .zsync file", source)
	}

	req, err := http.NewRequest("GET", source, nil)
	if err != nil {
		return nil, fmt.Errorf("failed to form HTTP request: %w", err)
	}
	if referer != "" {
		req.Header.Set("Referer", referer)
	}
	if auth, ok := auths[u.Hostname()]; ok {
		req.SetBasicAuth(auth.username, auth.password)
	}

	resp, err := client.Do(req)
	if err != nil {
		return nil, fmt.Errorf("HTTP request failed: %w", err)
	}
	defer resp.Body.Close()
	if resp.StatusCode != http.StatusOK {
		return nil, fmt.Errorf("failed to download .zsync: %s", resp.Status)
	}

	pathToUse := keepZsync
	var tmpFile *os.File
	if pathToUse == "" {
		tmpFile, err = os.CreateTemp("", "zsync-*.zsync")
		if err != nil {
			return nil, fmt.Errorf("temp file creation failed: %w", err)
		}
		defer tmpFile.Close()
		defer os.Remove(pathToUse)
	} else {
		tmpFile, err = os.Create(pathToUse)
		if err != nil {
			return nil, fmt.Errorf("zsync local file creation failed: %w", err)
		}
		defer tmpFile.Close()
	}

	// Copy zsync file from response to temporary file, then seek back to the
	// start for reading.
	if _, err := io.Copy(tmpFile, resp.Body); err != nil {
		return nil, fmt.Errorf("write error: %w", err)
	}
	if _, err := tmpFile.Seek(0, io.SeekStart); err != nil {
		return nil, fmt.Errorf("seek: %w", err)
	}

	zs, err := zsync.New(tmpFile)
	return zs, err
}

func readSeedFile(zs *zsync.State, filename string, noProgress bool) error {
	f, err := os.Open(filename)
	if err != nil {
		return fmt.Errorf("could not open seed file %s: %w", filename, err)
	}
	defer f.Close()
	err = zs.SubmitSourceFile(f, !noProgress)
	return err
}

var httpBytesDownloaded int64

func fetchRemainingBlocks(client *http.Client, zs *zsync.State, referer string, auths authMap, noProgress bool) error {
	urls := zs.GetUrls()
	if len(urls) == 0 {
		return fmt.Errorf("no download URLs known")
	}

	failed := make([]bool, len(urls))
	remaining := len(urls)

	var err error
	for zs.Status() < 2 && remaining > 0 {
		try := rand.Intn(len(urls))
		if failed[try] {
			continue
		}
		if err = fetchRemainingBlocksFromURL(client, zs, urls[try], referer, auths, noProgress); err != nil {
			failed[try] = true
			fmt.Fprintf(os.Stderr, "failed to complete download from %s(%s): %v\n", urls[try], referer, err)
			remaining--
		}
	}

	if zs.Status() < 2 {
		return fmt.Errorf("could not complete download; most recent error was: %w", err)
	}
	return nil
}

func fetchRemainingBlocksFromURL(client *http.Client, zs *zsync.State, rawURL, referer string, auths authMap, noProgress bool) error {
	u, err := url.Parse(rawURL)
	if err != nil {
		return fmt.Errorf("invalid URL %s: %w", rawURL, err)
	}
	var absUrl *url.URL
	if !u.IsAbs() {
		if referer == "" {
			return fmt.Errorf("URL '%s' from the .zsync file is relative, but no referer URL is known", rawURL)
		}
		base, err := url.Parse(referer)
		if err != nil {
			return err
		}
		absUrl = base.ResolveReference(u)
	} else {
		absUrl = u
	}

	if !noProgress {
		fmt.Fprintf(os.Stderr, "downloading new blocks from %s:\n", absUrl.String())
	}

	ranges := zs.NeededByteRanges()
	if len(ranges) == 0 {
		return nil
	}

	g, ctx := errgroup.WithContext(context.Background())
	g.SetLimit(3)
	start := time.Now()
	var mu sync.Mutex
	for _, r := range ranges {
		g.Go(func() error {
			bytesReceived, err := fetchRange(ctx, zs, client, absUrl, auths, r)
			// Lock protecting httpBytesDownloaded
			mu.Lock()
			defer mu.Unlock()
			httpBytesDownloaded += bytesReceived
			if err != nil {
				return err
			}
			if !noProgress {
				got, total := zs.Progress()
				elapsed := time.Since(start)
				fmt.Fprintf(os.Stderr, "\r%s %3.1fMBps %02.1f%% of target obtained", elapsed.Truncate(time.Millisecond*100).String(), float64(httpBytesDownloaded)/elapsed.Seconds()/1000000.0, float64(got)/float64(total)*100)
			}
			return nil
		})
	}
	err = g.Wait()
	if !noProgress {
		fmt.Fprintf(os.Stderr, "\n")
	}
	return err
}

func fetchRange(ctx context.Context, zs *zsync.State, client *http.Client, url *url.URL, auths authMap, r zsync.ByteRange) (int64, error) {
	req, err := http.NewRequestWithContext(ctx, "GET", url.String(), nil)
	if err != nil {
		return 0, err
	}
	req.Header.Set("Range", fmt.Sprintf("bytes=%d-%d", r.Start, r.End))
	req.Header.Set("User-Agent", "zsync/"+version)
	if auth, ok := auths[url.Hostname()]; ok {
		req.SetBasicAuth(auth.username, auth.password)
	}
	resp, err := client.Do(req)
	if err != nil {
		return 0, err
	}
	defer resp.Body.Close()
	if resp.StatusCode != http.StatusPartialContent {
		return 0, fmt.Errorf("expected partial content from %s, got %s", url.String(), resp.Status)
	}
	return zs.SubmitTargetData(r.Start, resp.Body)
}

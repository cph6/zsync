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
	"crypto/tls"
	"errors"
	"flag"
	"fmt"
	"io"
	"math"
	"net/http"
	"net/url"
	"os"
	"path"
	"path/filepath"
	"strings"
	"sync"
	"time"
	"unicode"

	"github.com/cph6/zsync"
	"github.com/cph6/zsync/internal/httpbasic"
)

const version = "0.7.2"

// stringSlice for repeatable flag values.
type stringSlice []string

func (s *stringSlice) String() string {
	return strings.Join(*s, ",")
}

func (s *stringSlice) Set(value string) error {
	*s = append(*s, value)
	return nil
}

// Returns a suggested filename for the target file, given the source path of
// the zsync control file.
func getFilenameBase(source string) string {
	var name string
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

func getFilename(zs *zsync.Syncer, source string) string {
	// First try using the filename specified in the zsync control file.
	name := zs.TargetFilename()
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
		return fmt.Errorf("refusing to overwrite %s with zsync control file. Did you use -k on the wrong file?", filename)
	}
	return nil
}

func main() {
	var (
		auths      = make(httpbasic.AuthMap)
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

	client := httpbasic.Client{
		Client: http.Client{
			Transport: &http.Transport{
				ForceAttemptHTTP2: true,
				Proxy:             http.ProxyFromEnvironment,
				TLSClientConfig:   &tls.Config{InsecureSkipVerify: skipVerify},
			},
		},
		AuthByHost: auths,
		UserAgent:  "zsync/" + version,
	}

	if err := checkSuppliedFilename(keepZsync); err != nil {
		fmt.Fprintf(os.Stderr, "%v", err)
		os.Exit(3)
	}
	controlReader, referer, err := getZsyncControlFile(&client, source, keepZsync, referer)
	if err != nil {
		fmt.Fprintf(os.Stderr, "failed to fetch control file: %v", err)
		os.Exit(3)
	}
	zs, err := zsync.New(controlReader, filename)
	if err != nil {
		fmt.Fprintf(os.Stderr, "failed to parse control file: %v", err)
		os.Exit(3)
	}
	if err := controlReader.Close(); err != nil {
		fmt.Fprintf(os.Stderr, "failed to parse control file: %v", err)
		os.Exit(3)
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

	localUsed := int64(0)
	for _, file := range seedFiles {
		if !quiet {
			fmt.Fprintf(os.Stderr, "reading seed file %s:", file)
		}
		if err := readSeedFile(zs, file, quiet); err != nil {
			fmt.Fprintf(os.Stderr, "%v\n", err)
			os.Exit(1)
		}

		got, total := zs.Progress()
		localUsed = got
		if !quiet {
			fmt.Fprintf(os.Stderr, "Done reading %s. %02.1f%% of target obtained.      \n", file, float64(got)/float64(total)*100.0)
		}
		if zs.Status() == zsync.CompleteData {
			break
		}
	}

	if zs.Status() == zsync.NoData && !quiet {
		fmt.Fprintln(os.Stderr, "No relevent local data found - I will be downloading the whole file. If that's not what you want, CTRL-C out. You should specify the local file is the old version of the file to download with -i (you might have to decompress it with gzip -d first). Or perhaps you just have no data that helps download the file")
	}

	if err := zs.RenameFile(tempFile); err != nil {
		fmt.Fprintf(os.Stderr, "rename failed: %v\n", err)
		os.Exit(1)
	}

	var progressReporter *progressReporter
	var reportTargetProgress func(url string, event zsync.FetchEvent, err error)
	if !quiet {
		progressReporter = NewProgressReporter(zs)
		reportTargetProgress = func(url string, event zsync.FetchEvent, err error) {
			progressReporter.progress(url, event, err)
		}
	}

	httpBytesDownloaded, fetchErr := zs.FetchRemainingBlocks(&client, referer, reportTargetProgress)
	if fetchErr != nil || zs.Status() != zsync.CompleteData {
		errMsg := ""
		if fetchErr != nil {
			errMsg = fetchErr.Error()
		}
		fmt.Fprintf(os.Stderr, "Not all of the required data could be downloaded, and the remaining data could not be retrieved from any of the download URLs. %s\n", errMsg)
		fmt.Fprintf(os.Stderr, "%s. Incomplete transfer left in %s.\n", "completed download left in", tempFile)
		os.Exit(3)
	}
	if !quiet {
		progressReporter.Close()
	}

	if verbose {
		s := zs.RStats()
		fmt.Printf("hash stats: bithash hit %d, hash hit %d, hash false positive %d, weak hit %d, checksums calculated %d, strong hit %d\n", s.BithashHit, s.HashHit, s.HashFalsePositive, s.WeakHit, s.Checksummed, s.StrongHit)
	}
	if !quiet {
		fmt.Print("verifying download...")
	}
	if err := zs.Complete(); err != nil {
		fmt.Fprintf(os.Stderr, "failed(%v), download available in %s\n", err, tempFile)
		os.Exit(2)
	}
	if !quiet {
		fmt.Println("checksum matches OK")
	}

	if filename != "" {
		oldBackup := filename + ".zs-old"

		if _, err := os.Stat(filename); err == nil {
			_ = os.Remove(oldBackup)
			if err := os.Link(filename, oldBackup); err != nil {
				if err2 := os.Rename(filename, oldBackup); err2 != nil {
					fmt.Fprintf(os.Stderr, "Unable to back up old file %s - completed download left in %s\n", filename, zs.Filename())
					os.Exit(2)
				}
			}
		}
	}

	finalFilename, err := zs.End(filename)
	if err != nil {
		fmt.Fprintf(os.Stderr, "failed(%v), download available in %s\n", err, finalFilename)
		os.Exit(2)
	} else if filename == "" {
		fmt.Printf("No filename specified for download - completed download left in %s\n", finalFilename)
	}

	if !quiet {
		fmt.Printf("used %d local, fetched %d\n", localUsed, httpBytesDownloaded)
	}
}

// getZsyncControlFile wraps zsync.GetControlFile giving the option to use a
// local file instead of fetching remotely.
func getZsyncControlFile(client zsync.HTTPRequester, source, keepZsync, referer string) (io.ReadCloser, string, error) {
	// First try to read from local file.
	if _, err := os.Stat(source); err == nil {
		f, err := os.Open(source)
		if err != nil {
			return nil, "", fmt.Errorf("failed to open .zsync: %w", err)
		}
		// If the control file is local, we use the supplied referer URL as the
		// referer for the target file requests.
		return f, referer, nil
	}

	return zsync.GetControlFile(client, source, keepZsync, referer)
}

func readSeedFile(zs *zsync.Syncer, filename string, noProgress bool) error {
	f, err := os.Open(filename)
	if err != nil {
		return fmt.Errorf("could not open seed file %s: %w", filename, err)
	}

	var bytesObtainededAtLastProgress, offsetAtLastProgress int64

	pf := func(offset int64) {
		if offset >= offsetAtLastProgress+(1<<20) {
			bytesObtained, _ := zs.Progress()
			useFraction := float64(bytesObtained-bytesObtainededAtLastProgress) / float64(offset-offsetAtLastProgress)
			progressDecile := min(9, int(math.Ceil(useFraction*10)))
			fmt.Fprintf(os.Stderr, "%d", progressDecile)
			offsetAtLastProgress = offset
			bytesObtainededAtLastProgress = bytesObtained
		}
	}
	if noProgress {
		pf = nil
	}

	seedSink := zs.NewSeedSink(pf)
	_, err = seedSink.ReadFrom(f)
	if err != nil {
		return err
	}
	if closeErr := f.Close(); closeErr != nil {
		return fmt.Errorf("failed to close seed file: %w", closeErr)
	}
	return nil
}

type progressReporter struct {
	zs         *zsync.Syncer
	startTime  time.Time
	startBytes int64
	ticker     *time.Ticker
	stopTicker chan struct{}
	mu         sync.Mutex
}

func NewProgressReporter(zs *zsync.Syncer) (p *progressReporter) {
	p = &progressReporter{
		zs:         zs,
		ticker:     time.NewTicker(time.Second),
		stopTicker: make(chan struct{}),
	}
	go p.ShowProgressLoop()
	return
}

func (p *progressReporter) Close() {
	p.ticker.Stop()
	close(p.stopTicker)
}

func (p *progressReporter) ShowProgressLoop() {
	for {
		select {
		case <-p.ticker.C:
			p.mu.Lock()
			p.showProgress()
			p.mu.Unlock()
		case <-p.stopTicker:
			return
		}
	}
}

func (p *progressReporter) showProgress() {
	got, total := p.zs.Progress()
	elapsed := time.Since(p.startTime)
	fmt.Fprintf(os.Stderr, "\r%s %3.1fMBps %02.1f%% of target obtained", elapsed.Truncate(time.Millisecond*100).String(), float64(got-p.startBytes)/elapsed.Seconds()/1000000.0, float64(got)/float64(total)*100)
}

func (p *progressReporter) progress(url string, event zsync.FetchEvent, err error) {
	p.mu.Lock()
	defer p.mu.Unlock()
	if err != nil {
		fmt.Fprintf(os.Stderr, "failed to complete download from %s: %v\n", url, err)
		return
	}
	switch event {
	case zsync.FetchStarted:
		p.startTime = time.Now()
		p.startBytes, _ = p.zs.Progress()
		fmt.Fprintf(os.Stderr, "downloading new blocks from %s:\n", url)
	case zsync.FetchEnded:
		p.showProgress()
		fmt.Fprintf(os.Stderr, "\n")
	}
}

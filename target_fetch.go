package zsync

import (
	"context"
	"fmt"
	"math/rand"
	"net/http"
	"net/url"
	"sync"

	"golang.org/x/sync/errgroup"
)

/*
 * SPDX-FileCopyrightText: 2004,2005,2007,2009,2025,2026 Colin Phipps <cph@moria.org.uk>
 *
 * SPDX-License-Identifier: Artistic-2.0
 */

// AI: parts from copilot / Raptor mini conversion of zsync's main.c, but with
// considerable simplifcation compared to the original client: no longer
// putting multiple ranges in one request, no pipelining.

// HTTPRequester is a subset of http.Client that zsync needs for HTTP.
// Callers may provide an *http.Client directly (it implements Do) or a
// wrapper that implements Do to provide authentication or headers.
type HTTPRequester interface {
	Do(req *http.Request) (*http.Response, error)
}

// FetchEvent represents the type of a progress event from FetchRemainingBlocks
type FetchEvent int

const (
	FetchStarted FetchEvent = iota
	FetchEnded
)

// FetchRemainingBlocks attempts to complete the target file by
// downloading any missing blocks using the supplied HTTPRequester.
// It uses the URLs specified in the zsync control file.
// progressCallback, if supplied, will be called:
//   - at the start of downloading from each source,
//   - at the end of downloading from each source with err=io.EOF on no error, or
//     otherwise with the error encountered.
func (zs *Syncer) FetchRemainingBlocks(client HTTPRequester, referer string, progressCallback func(url string, event FetchEvent, err error)) (httpBytesDownloaded int64, err error) {
	if len(zs.urls) == 0 {
		return httpBytesDownloaded, fmt.Errorf("no download URLs known")
	}

	failed := make([]bool, len(zs.urls))
	remaining := len(zs.urls)

	for zs.Status() != CompleteData && remaining > 0 {
		try := rand.Intn(len(zs.urls))
		if failed[try] {
			continue
		}

		url := zs.urls[try]
		if progressCallback != nil {
			progressCallback(url, FetchStarted, nil)
		}
		var fetched int64
		fetched, err = zs.fetchRemainingBlocksFromURL(client, url, referer)
		if err != nil {
			failed[try] = true
			remaining--
		}
		httpBytesDownloaded += fetched
		if progressCallback != nil {
			progressCallback(zs.urls[try], FetchEnded, err)
		}
	}

	if zs.Status() != CompleteData {
		err = fmt.Errorf("could not complete download; most recent error was: %w", err)
	}
	return
}

func (zs *Syncer) fetchRemainingBlocksFromURL(client HTTPRequester, rawURL, referer string) (int64, error) {
	u, err := url.Parse(rawURL)
	if err != nil {
		return 0, fmt.Errorf("invalid URL %s: %w", rawURL, err)
	}
	var absURL *url.URL
	if !u.IsAbs() {
		if referer == "" {
			return 0, fmt.Errorf("URL '%s' from the .zsync file is relative, but no referer URL is known", rawURL)
		}
		base, err := url.Parse(referer)
		if err != nil {
			return 0, err
		}
		absURL = base.ResolveReference(u)
	} else {
		absURL = u
	}

	ranges := zs.NeededByteRanges()
	if len(ranges) == 0 {
		return 0, nil
	}

	g, ctx := errgroup.WithContext(context.Background())
	g.SetLimit(3)

	var (
		httpBytesDownloaded int64
		mu                  sync.Mutex
	)

	for _, r := range ranges {
		g.Go(func() error {
			bytesReceived, err := zs.fetchRange(ctx, client, absURL, r)
			// Lock protecting httpBytesDownloaded.
			mu.Lock()
			defer mu.Unlock()
			httpBytesDownloaded += bytesReceived
			if err != nil {
				return err
			}
			return nil
		})
	}
	return httpBytesDownloaded, g.Wait()
}

func (zs *Syncer) fetchRange(ctx context.Context, client HTTPRequester, url *url.URL, r ByteRange) (int64, error) {
	req, err := http.NewRequestWithContext(ctx, "GET", url.String(), nil)
	if err != nil {
		return 0, err
	}
	req.Header.Set("Range", fmt.Sprintf("bytes=%d-%d", r.Start, r.End))
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

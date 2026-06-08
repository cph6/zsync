package zsync

/*
 * SPDX-FileCopyrightText: 2004,2005,2007,2009,2025,2026 Colin Phipps <cph@moria.org.uk>
 *
 * SPDX-License-Identifier: Artistic-2.0
 */

// AI: from copilot / Raptor mini conversion of zsync's main.c, but reworked
// significantly.

import (
	"fmt"
	"io"
	"net/http"
	"net/url"
	"os"
	"time"
)

// GetControlFile reads a zsync control file using the
// provided HTTPRequester. If keepZsync is non-empty, the control file is also
// cached locally. The returned string is the referer URL that should be used
// when resolving any relative URLs contained in the control file.
// This is a helper function that is not required to use the rest of the
// module; you can use a local zsync control file or download a control file
// with a different client. This is essentially just a HTTP downloader with
// mtime/If-Modified-Since caching..
func GetControlFile(client HTTPRequester, source, keepZsync, referer string) (io.ReadCloser, string, error) {
	u, err := url.Parse(source)
	if err != nil || u.Scheme == "" {
		return nil, "", fmt.Errorf("%s is not a valid URL", source)
	}

	var fileInfo os.FileInfo
	gotMtime := false
	if keepZsync != "" {
		fileInfo, err = os.Stat(keepZsync)
		if err != nil && !os.IsNotExist(err) {
			fmt.Fprintf(os.Stderr, "failed to get mtime for existing .zsync control file: %v", err)
			// Fall through with no mtime - we can just download the control file.
		} else if err == nil {
			gotMtime = true
		}
	}

	req, err := http.NewRequest("GET", source, nil)
	if err != nil {
		return nil, "", fmt.Errorf("failed to form HTTP request: %w", err)
	}
	if referer != "" {
		req.Header.Set("Referer", referer)
	}
	if gotMtime {
		req.Header.Set("If-Modified-Since", fileInfo.ModTime().UTC().Format(time.RFC1123))
	}

	resp, err := client.Do(req)
	if err != nil {
		return nil, "", fmt.Errorf("HTTP request failed: %w", err)
	}
	if keepZsync != "" && resp.StatusCode == http.StatusNotModified {
		// Not modified, so we can drop the response and read the local copy.
		fmt.Fprintf(os.Stderr, "control file not modified - using local copy\n")
		_ = resp.Body.Close()
		f, err := os.Open(keepZsync)
		return f, source, err
	}
	// Otherwise we need the response.
	if resp.StatusCode != http.StatusOK {
		_ = resp.Body.Close()
		return nil, "", fmt.Errorf("failed to download .zsync: %s", resp.Status)
	}

	// If we are not saving a local copy of the .zsync file, we can just
	// pass the response body reader to the caller.
	if keepZsync == "" {
		return resp.Body, source, nil
	}

	controlFile, err := os.Create(keepZsync)
	if err != nil {
		return nil, "", fmt.Errorf("zsync local file creation failed: %w", err)
	}
	// Copy zsync file from response to temporary file, then seek back to the
	// start for reading.
	if _, err := io.Copy(controlFile, resp.Body); err != nil {
		// We already truncated the local copy of the control file, so
		// invalidate it here by deleting it - the code above assumes that
		// any existing file is a valid download for If-Modified-Since.
		_ = os.Remove(keepZsync)
		return nil, "", fmt.Errorf("write error: %w", err)
	}
	_ = resp.Body.Close()
	if _, err := controlFile.Seek(0, io.SeekStart); err != nil {
		return nil, "", fmt.Errorf("seek: %w", err)
	}

	// Set the mtime to the Last-Modified from the server if available,
	// so that we supply the correct If-Modified-Since on any rerun.
	if lastModified, ok := resp.Header["Last-Modified"]; ok && len(lastModified) > 0 {
		if err := setMTime(keepZsync, lastModified[0]); err != nil {
			fmt.Fprintf(os.Stderr, "warning: failed to set mtime of control file (%v); this may lead to incorrect caching of the zsync control file\n", err)
		}
	}
	// TODO: the referer URL returned here should be the final URL
	// requested after redirects, not the initial URL.
	return controlFile, source, nil
}

func setMTime(filename string, mTimeStr string) error {
	mtime, err := time.Parse(time.RFC1123, mTimeStr)
	if err != nil {
		return err
	}
	return os.Chtimes(filename, time.Now(), mtime)
}

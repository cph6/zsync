// Package httpbasic is a thin wrapper around an http.Client that adds
// basic auth headers and sets the user agent on each request.
package httpbasic

import (
	"fmt"
	"net/http"
	"strings"
)

/*
 * SPDX-FileCopyrightText: 2004,2005,2007,2009,2025,2026 Colin Phipps <cph@moria.org.uk>
 *
 * SPDX-License-Identifier: Artistic-2.0
 */

// AI: from copilot / Raptor mini conversion of zsync's main.c.

// Client is a thin wrapper around an http.Client which adds HTTP basic auth.
// The caller is expected to build this themselves from the contained types.
type Client struct {
	Client     http.Client
	AuthByHost AuthMap
	UserAgent  string
}

func (c *Client) Do(req *http.Request) (*http.Response, error) {
	if auth, ok := c.AuthByHost[req.URL.Hostname()]; ok {
		req.SetBasicAuth(auth.username, auth.password)
	}
	req.Header.Set("User-Agent", c.UserAgent)
	return c.Client.Do(req)
}

type authCred struct {
	username string
	password string
}

// AuthMap is usable as a flag type which provides HTTP basic auth credentials
// per hostname.
type AuthMap map[string]authCred

func (a AuthMap) String() string {
	entries := make([]string, 0, len(a))
	for host, cred := range a {
		entries = append(entries, fmt.Sprintf("%s=%s:%s", host, cred.username, cred.password))
	}
	return strings.Join(entries, ",")
}

func (a AuthMap) Set(value string) error {
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

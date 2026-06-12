# AGENTS.md — zsync Developer Guide

## Quick Facts

**zsync** is a file transfer program (Go implementation) that downloads only changed blocks of a file using rolling checksums and a control file (`.zsync`). Two binaries: `zsync` (client) and `zsyncmake` (server-side control file generator).

- **Language**: Go 1.25.0
- **Module**: `github.com/cph6/zsync`
- **License**: Artistic 2.0
- **Maintainer**: Colin Phipps <cph@moria.org.uk>

## Build

```bash
go build -o zsync ./cmd/zsync
go build -o zsyncmake ./cmd/zsyncmake
```

Binary entrypoints: `cmd/zsync/main.go`, `cmd/zsyncmake/main.go`

## Architecture

- **`syncer.go`** / **`syncer_test.go`**: Core Syncer library—loads `.zsync` control files, reconstructs target files from local seed + remote blocks. Primary public API. Not thread-safe except `Status()` and `Progress()`.
- **`control_file_parse.go`**: Parse `.zsync` control file format (text headers + binary checksums).
- **`control_file_fetch.go`**: Fetch target file blocks via HTTP(S).
- **`target_fetch.go`**: Download file blocks, handle ranges, retries.
- **`internal/rcksum/`**: Rolling checksum + MD4 hashing (core sync algorithm).
- **`internal/httpbasic/`**: HTTP Basic Auth support.
- **`cmd/zsync/main.go`**: CLI for end-users. Wraps Syncer library.
- **`cmd/zsyncmake/main.go`**: CLI to generate `.zsync` control files from target files.

Key library types:
- `Syncer`: State machine for reconstruction; manage with `New()`, `FetchRemainingBlocks()`, `Complete()`, `End()`.
- `SeedReader`: Feed local file data to match against existing blocks.
- `HTTPRequester`: HTTP client interface (allows test injection).

## Testing

**Short tests (unit):**
```bash
go test -short ./...
```

**Full integration tests** (requires Apache, OpenSSL, tinyproxy):
```bash
LARGE_TESTS=yes go test ./...
```

**Single test:**
```bash
go test -v -run TestZSyncMakeSimple ./t/zsync
```

**Test structure:**
- Unit tests in `*_test.go` alongside source (e.g., `syncer_test.go`, `internal/rcksum/*_test.go`).
- Integration tests in `t/zsync/` and `t/zsyncmake/` with Apache HTTP server, TLS certs, proxy support.
- Test data in `t/data/` (generated); logs in `t/logs/`.
- `t/server_setup.go`: Spawns Apache, sets up HTTPS, proxy.
- Note: integration tests are slow; run `-short` for quick feedback.

## Code Comments & Attribution

- Many functions include `// AI: <model>` comments marking AI-assisted code sections (e.g., GPT, Copilot, Claude).
- Preserved from original C codebase; use these as context hints when refactoring or extending logic.
- SPDX headers on all files; maintain them.

## Linting & Style

```bash
golangci-lint run
```

Config: `.golangci.yaml` (staticcheck with some exceptions: `-SA1019` for deprecated code, `-QF1001` for questionable format).

## Notable Quirks & Constraints

1. **API Stability**: v0.x version number—public API may change. NEWS file tracks breaking changes (v0.7.0 was major rewrite from HTTP pipelining to HTTP/2).
2. **Library Mode**: As of v0.7.2, zsync is usable as a Go module (Syncer exported as public API). CLI is thin wrapper around library.
3. **Control File Format**: Binary checksums follow text headers; see `control_file_parse.go` for format details (rsum, blocksize, hash lengths, etc.).
4. **HTTP Ranges**: Uses HTTP 206 Partial Content; `target_fetch.go` handles range requests and retries.
5. **Testing Preconditions**: Full tests need Apache, OpenSSL, tinyproxy installed (even if not running—tests spawn them). Proxy tests need local IP (not localhost) due to Go HTTP client proxy rules.
6. **Man Pages**: In `man/` (zsync.1, zsyncmake.1); update if CLI changes.

## Common Commands for Agents

| Task | Command |
|------|---------|
| Build both binaries | `go build -o zsync ./cmd/zsync && go build -o zsyncmake ./cmd/zsyncmake` |
| Run unit tests | `go test -short ./...` |
| Run specific test | `go test -v -run TestName ./path/to/package` |
| Full test suite (slow) | `LARGE_TESTS=yes go test ./...` |
| Lint | `golangci-lint run` |
| Check module deps | `go mod tidy && git diff go.* ` |

## Recent History

- v0.7.2 (current): Fixed stdin crash in zsyncmake, continual progress reporting, library-mode support.
- v0.7.0: Complete rewrite—dropped HTTP pipelining, added HTTP/2 + HTTPS, removed gzip introspection.
- Extensive refactoring since—see git log for details.

## Known Maintenance Notes

- Gzip files: only supported if compressed with `--rsyncable`; legacy gzip introspection removed.
- Proxy tests may be flaky on systems without clear local IP; see `t/zsync/server_setup.go`.

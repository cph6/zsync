# rcksum

## Overview

The rcksum module implements the rsync algorithm for identifying blocks of data
that are common between two files. It's designed to work with zsync's file
synchronization tool to allow a target file to be reconstructed using data from
old local file(s) + downloading missing blocks from a server.

This is a complete Go port of the rsync rolling checksum library from the C version
of zsync.

## Copyright Notice

Copyright © 2004,2005,2007,2009,2025,2026 Colin Phipps <cph@moria.org.uk>

This module is free software; you can redistribute it and/or modify it under the
terms of the Artistic License v2 (see the accompanying file COPYING for the full
license terms), or, at your option, any later version of the same license.

This program is distributed in the hope that it will be useful, but WITHOUT ANY
WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS FOR A
PARTICULAR PURPOSE.  See the COPYING file for details.

## Features

- **Rolling Checksum**: Fast, byte-by-byte scanning using Adler-style checksums.
- Hash table and lookup for rolling checksums, to allow quick comparison of
  matching blocks.
- **Strong Checksums**: MD4-based verification of potential matches.
- **Range Tracking**: to keep track of which blocks have been received.
- **File Handling and handoff of temporary file created during reconstruction.

## Usage

### Creating a State Object

```go
import "github.com/cph/zsync/internal/rcksum"

// Create state for 100 blocks of 4096 bytes each
z, err := rcksum.New(100, 4096, 2, 16, 1)
if err != nil {
    log.Fatal(err)
}
defer z.Close()
// Provide scratch file; the caller takes over this file once it is finished
// with rcksum, and will probably want to rename it after a successful
// reconstruction.
f, err := os.CreateTemp("", "rcksum-XXXXXX")
z.SetTargetFile(f)
```

### Matching Source Data

```go
// Process source file
f, _ := os.Open("source.bin")
matched, err := z.SubmitSourceFile(f, /* showProgress= */ true)
fmt.Printf("Matched %d blocks\n", matched)
```

### Checking Progress

```go
remaining := z.BlocksTodo()
fmt.Printf("%d blocks remaining\n", remaining)

// Get ranges of blocks still needed
needed := z.NeededBlockRanges(0, z.blocks)
```

## Implementation Details

### Files

- **types.go** - Core type definitions
- **rcksum.go** - Main API and initialization
- **hash.go** - Hash table construction and lookups
- **ranges.go** - Range tracking of received blocks
- **submit.go** - Data submission and block matching

### Core Types

RcksumState - The main state object that holds:

  - Block checksums (both weak rolling checksums and strong MD4 hashes)
  - Hash tables for fast block lookup
  - Tracking of known blocks
  - Temporary file for accumulating received data

RSum - A rolling Adler-style checksum.

### Algorithms

#### Rolling Checksum (Weak)

Uses an Adler-32-style checksum that updates incrementally:
- `a` = sum of all bytes
- `b` = sum of `i * byte[i]` for all positions

#### Strong Checksum

MD4 digest of block data for collision verification

#### Hash Lookup

Hybrid approach with:
- Bitmask (bithash) for O(1) negative lookups
  - tested on MX-25.1_Xfce_x64.iso -> MX-25.2_Xfce_x64.iso; the bithash reduced
    total CPU cost during the seed file stage from ~35s to ~25s. So it is
    still a significant saving on top of the golang's native maps.
- Hash chains for collision resolution

## Dependencies

- `golang.org/x/crypto/md4` - MD4 hashing algorithm

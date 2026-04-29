# rcksum - Go Implementation

This is a complete Go port of the rsync rolling checksum library from zsync.

## Overview

The rcksum library implements the rsync algorithm for identifying blocks of data that are common between two files. It's designed to work with zsync's file synchronization tool.

## Features

- **Rolling Checksum**: Fast, byte-by-byte scanning using Adler-style checksums
- **Strong Checksums**: MD4-based verification of potential matches
- **Hash Tables**: Optimized lookup structures with bitmask for fast negative lookups
- **Range Tracking**: Efficient tracking of which blocks have been received
- **Thread-Safe**: All operations are protected by mutexes
- **File Handling**: Automatic management of temporary files

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
```

### Adding Target Blocks

```go
// For each block in the target file:
blockData := readBlock(targetFile, blockID)
r := rcksum.CalcRsumBlock(blockData)
checksum := rcksum.CalcChecksum(blockData)
z.AddTargetBlock(blockID, r, checksum)

// Build hash tables for matching
err := z.BuildHash()
```

### Matching Source Data

```go
// Process source file
matched := z.SubmitSourceData(sourceData, offset)
fmt.Printf("Matched %d blocks\n", matched)

// Or from a file reader:
f, _ := os.Open("source.bin")
matched, _ := z.SubmitSourceFile(f)
```

### Checking Progress

```go
remaining := z.BlocksRemaining()
fmt.Printf("%d blocks remaining\n", remaining)

// Get ranges of blocks still needed
needed := z.NeededBlockRanges(0, z.blocks)
```

### Retrieving Results

```go
// Get the temporary file
fd := z.File()
defer fd.Close()

// Get the temporary filename
filename := z.Filename()
```

## Implementation Details

### Files

- **types.go** - Core type definitions
- **rcksum.go** - Main API and initialization
- **hash.go** - Hash table construction and lookups
- **ranges.go** - Range tracking of received blocks
- **submit.go** - Data submission and block matching
- **rcksum_test.go** - Unit tests

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
- Hash chains for collision resolution
- Load factor optimization

### Performance

The implementation achieves good performance through:
- Efficient rolling checksum updates (O(1) per byte)
- Fast negative lookups via bitmask
- Lazy hash table construction
- Minimal memory copying

## Comparison with C Implementation

This Go implementation maintains API compatibility and algorithmic equivalence with the original C implementation while adapting to Go idioms:

- Replaces manual memory management with Go's garbage collector
- Uses channels and goroutines instead of callbacks where appropriate
- Replaces C macros with Go functions
- Thread-safe by default with sync.Mutex
- Idiomatic error handling with error returns

## Testing

Run tests with:
```bash
go test -v ./internal/rcksum
```

Run benchmarks with:
```bash
go test -bench=. ./internal/rcksum
```

## Dependencies

- `golang.org/x/crypto/md4` - MD4 hashing algorithm

## License

This implementation maintains the same license as the original zsync project.

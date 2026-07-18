# zsync File Specification

Specification version: 0.8

## Introduction

zsync is a file transfer process and a synonymous client program that allows a
file to be downloaded by downloading only the parts of the file that the user
does not already have. It uses a control file together with previous versions
or alternative files that share data with the target file to fill in parts of
the target file, and then download the remainder.

A zsync control file (conventionally file extension `.zsync`) is to zsync what
a `.torrent` is for bittorrent clients.

The original zsync included a program to generate the control files. Back in
the 2000s there was only one zsync client (https://zsync.moria.org.uk/) and so
that defined the format of the control file and set the process used. But these
were treated as all internal details of how the client and generator
implemented the zsync process; an example control file was included in the
technical paper but without definitions or a specification.

More recently, forks and alternative implementations have appeared. These have
used the same file format as the original zsync (at least by default) and
interoperability was generally defined by interoperability with the original
zsync; alternative implementations reverse engineered the file format from the
original zsync code.

This specification defines a file format for zsync.

Note that this document does NOT seek to define any process, strict or
otherwise, as to how the file reconstruction is done by the zsync client,
though the "system overview" section gives a brief overview. It only specifies
the data in the control file and how the client should use specific fields. The
zsync technical paper describes how the official client does it and enabling
that process is the purpose of the zsync file, but other clients are free to
approach the problem of file reconstruction differently.

### Status

The original zsync remains pre-version-1.0 software, even if it has seen some
significant adoption — mainly by Linux distributions. As such this
specification is also pre-version-1 and may change over time. I am interested
in feedback from other implementors on any problems or opportunities that they
see in the specification.

As the author of the original zsync, I will ensure that the original zsync
client and generator follow this format; and the forks and alternative
implementations then have something to work to if they wish to remain
interoperable without needing to reverse engineer the details.

While I hope that writing this specification and ensuring that the original
zsync works well with it will stabilise the format and allow both this
specification and zsync to reach a stable version, I have no particular
timeline in mind at this time for reaching a final specification.

### Notation

This specfiication will use the words MUST, SHALL, MAY, OPTIONAL and variations
thereof with the same meanings as an RFC.

## System Overview

The zsync process involved 4 major components:

- a web server
- a target file
- a zsync control file
- a zsync client program

The target file is just the file, any file that the user wants to download. In
principle any web server supporting RFC7233 can act as the server; typically
this server hosts both the target file and the zsync control file, though they
can also be on different servers. The zsync client program is the program that
implements the zsync process and algorithm.

The control file is only used by zsync in order to perform the download
process. A control file must be generated specific to the target file; it
provides the information used by the client to:

- identify the blocks of target data that it already has locally,
- download locations for missing data,
- checksums to verify that the target file is built successfully.

Typically the distributor of the target file will generate a zsync control file
and place it alongside the target file, allowing users to either download the
full file or to use zsync to update to the file from an earlier version.

The zsync process, at at high level, consists of:

- the client downloads the control file (from a URL supplied by the user).
- the control file provides block checksums for the data in the target file;
  the zsync client reads through the locallly-supplied data from the user and
  compares checksums of local data against the target checksums.
- blocks of target data that are found locally are written to the output file.
  The zsync client program keeps track of which block are known and have been
  written, and which blocks are still unknown.
- the control file also includes URL(s) for the target file. The zsync client
  program downloads the missing blocks of data from one of these URLs.
- once all target blocks are obtained and written, the zsync client program
  rereads the data and verifies the whole download against the whole-file target
  checksum in the zsync control file, to check that reconstruction was
  successful.

### Terminology

- target data is data that occurs in the target file.
- the zsync generator is the program that generates the control file.
- the zsync client is the program that the user runs in order to obtain the
  target file.
- blocks are contiguous byte ranges in the target file; the generator breaks
  the target file down into blocks when generating the control file.
- target checksums or just "checksums" is used loosely to refer to checksum or
  hashes for blocks of target data.
- weak checksum is an adler32-like checksum, specified below.
- strong checksum is a more collision-resistant checksum, specified below.

## File format

### Overall structure

A zsync control file consists of two sections:

```
+-----------+
|  HEADERS  |
+-----------+
| CHECKSUMS |
+-----------+
```

There is no outer structure around this; the headers start at the start of the
control file and, at the end of the headers, the checksums immediately follow.

### Headers

The headers section of the control file provides metadata about the target
file. It consists of one or more lines of utf-8 text, each line being one
header. An empty line indicates the end of the header section.

```
zsync-headers = *( header-field CR) CR

header-field = field-name ": " field-value
```

(Informally, the header section resembles HTTP headers, but with no
request/status line, no line feeds, and limited to a simpler syntax overall.)

There is no limit to the number of header-fields, though practically it is not
expected to exceed 10. Repeated headers (headers-fields with the same name and
potentially different values) may be present only for certain field-names as
specified below; if so, order of repeats of a header is not significant. Order
between header fields is generally not significant unless stated otherwise
below.

Below the header field-names that may be present are defined. Clients MAY ignore any
header field not defined in this specification. TODO: should clients fail on
unrecognised headers, as zsync has always done?

#### "zsync"

The first header-field in the headers section is always the "zsync" header. A
file without the zsync header-field MUST NOT be interpreted as a zsync control
file. In practice, this means that all zsync control files start with the byte
sequence "zsync: ".

zsync generator programs SHOULD include a value for this header that identifies
the generator program. No particular format is defined for this field, but
clients MAY choose to follow the following format:

```
zsync-value = program-name "/" version-number
```

(Informally, the original zsync just writes its version number without
prefixing the program name.)

zsync clients can ignore the value of the zsync header-field. Or clients MAY
choose to use the value in the field to enable compatibility workarounds to
interoperate with particular generators that may deviate from this
specification.

#### Filename

The Filename header-field gives a suggested filename for the target file to be
created. zsync clients MAY rename the target file after complete reconstruction
to the name from the Filename field.

zsync generators SHOULD include the filename field and set it to the filename
that the user would expect for the file if the name is known; typically this is
simply the name of the file given to the generator as the target file.

The generator MUST NOT generate a filename header-field value containing "/".
The value also cannot include a CR even if that is present in the filename (on
systems that allow this) since a CR terminates the header-field.

Clients SHOULD ignore any Filename value that would cause it to write to a file
outside of the directory that the user might expect the file to be created in.
zsync clients MAY choose to ignore any filename that appears to violate the
principle of least surprise.

(Informally this field is to ease use of the zsync client by allowing users to
tell the client only the URL of the zsync file without also specifying the name
of the file to be created, and having the zsync client get the name from the
control file.)

#### MTime

The MTime field is optional. If present, its value is an RFC1123 timestamp.
zsync generators SHOULD include the MTime header set to the modification time
of the target file. zsync clients MAY set the modification time of the target
file after reconstruction to the time from the MTime field.

#### Blocksize

The blocksize field is required, and specifies the size of the target data
blocks used in the control file, in particular for the data in the CHECKSUMS
section. The blocksize is an integer and MUST be a power of 2.

#### Length

The length field specifies the length in bytes of the target file. zsync
generators MUST include this field set to the length of the target file.

#### File-Hash

The File-Hash field value has the following format:

```
file-hash-value = algorithm ":" digest
```

algorithm is a string identifing a message digest algorithm; the value SHOULD
be one of "SHA-1" or "SHA-256". The digest value is the hexadecimal
representation of a digest calculated simply over the entire target file.

TODO: should repeated File-Hash headers be allowed (to include digests with
different algorithms).

#### URL

The URL header-field is a repeated header. Each URL value is a URL from which
the target file can be obtained in full.

URL values may be absolute URLs or relative URLs; if relative, zsync clients
SHOULD interpret the URL relative to the location from which the zsync file was
retrieved. If a zsync client supports reading a local copy of a zsync file, it
SHOULD provide a way for the user to specify the URL from which it was
originally downloaded so that the contained URLs can be interpreted relative to
that location.

zsync generators SHOULD include at least one URL from which the target file is
expected to be able to be obtained.

If multiple URLs are given, the zsync client is free to choose in any way which
URL(s) to use and in what order. zsync clients SHOULD discard URLs that return
errors and switch to a different URL from the control file and only return an
error if downloading is not successful from any of the provided URLs.

#### Hash-Lengths

The Hash-Lengths field provides parameters for the zsync reconstruction process. The value has the format:

```
hash-lengths-value = seq_matches "," rsum_length "," strong_hash_length
```

where:

- seq_matches is either "1" or "2". zsync generators SHOULD set seq_matches to
  "2" if multiple consecutive weak-checksum-matching blocks are required to have
  a high confidence of matching data (either due to a value of less than 4 for
  rsum_length, or due to the size of the file). zsync clients MAY choose to
  follow this advice or to perform their own assessment of the risk of rsum
  collisions.
- rsum_length is "2", "3" or "4" and specifies the number of bytes per block of
  rsum data supplied in the CHECKSUMS section.
- strong_hash_length is an integer string with a value between 1 and 32. It
  specified the number of bytes of strong hash data supplied per block in the
  CHECKSUMS section.

If the Hash-Lengths header is not present in the headers, the zsync client MUST
assume:

- seq_matches unspecified,
- rsum_length is 4,
- strong_hash_length is the length in bytes of the raw binary representation of
  a hash for the strong hash algorithm; that is, 16 for MD4 or MD5, 28 for
  SHA-224.

zsync generators MAY omit the Hash-Lengths header if those assumptions are met.

zsync generators MUST include the Hash-Lengths header unless the default
rsum_length and strong_hash_length values above are used in the CHECKSUMS
section. If the defaults are used, zsync generators may want to include the
header anyway if backwards compatibility with legacy versions of zsync is
desirable.

#### Strong-Hash-Algorithm

The value of this header is the textual identifier of the digest algorithm used
for the strong hashes in the CHECKSUMS section of the control file. It SHOULD
be one of "MD4", "MD5", "SHA-224".

zsync generators MUST include the Strong-Hash-Algorithm header unless the
digest used is MD4. If MD4 is used, generators may omit the header or use a
Safe header (below) to allow interoperability with older versions of zsync.
zsync clients SHOULD assume MD4 if the header is absent.

#### (Legacy) Safe

zsync client can ignore the Safe header.

For older versions of zsync, the safe header field is a comma-separated list of
field names that the zsync client MAY ignore. zsync generators MAY include a
Safe header listing headers that older zsync clients can ignore. Generators
MUST NOT include field names in a safe header if the generated zsync file
without that header would result in the header or checksum sections of the
zsync file not correctly being parsable by zsync-0.6 or otherwise make a
successful use by zsync-0.6 impossible.

#### (Legacy) Z-URL

zsync clients can ignore this header. zsync generators SHOULD NOT include this header.

Legacy: if present, this provides a URL to a gzip-compressed version of the
target file's contents.

#### (Legacy) Z-Map2

zsync generators SHOULD NOT include this header. Any zsync control file
containing this header is not compatible with this specification, and zsync
clients MAY reject any control file containing it. What follows is a
descripiong of the format of this header for clients that nevertheless want to
interoperate with control file from older zsync versions that may include this
header.

If present, Z-Map2 implies a break from the format of the header section as
described above. The header has the following format:

```
zmap2-header = "Z-Map2: " size CR zmap-data
zmap-data = (gzblock)*
```

where `size` is an integer giving the number of records attached, and zmap-data
is a set of `size` gzblock records. Each gzblock record is 4 bytes of binary
data. The purpose and meaning of the gzblock data is described in the original
zsync technical paper, and is left unspecified in this specification.

While zsync control files including this header do not confirm to this
specification, if the control file includes a URL header then it is typically
usable by a client that follows this specification. Therefore zsync clients MAY
choose to parse Z-Map2 headers and skip over the format-breaking inline binary
data following the header in order to parse the rest of the file and attempt to
use it.

#### (Legacy) SHA-1

zsync generators SHOULD NOT include this header. zsync clients can ignore this
header, or clients MAY use the value of this header field as the SHA1 file hash
of the target data (equivalent to `File-Hash: SHA1:value`).

### Checksum Section

```
checksum-section = (block-checksum)*
block-checksum = rolling-checksum strong-hash
rolling-checksum, strong-hash are binary data blobs of fixed size
```

The checksum section consists of serialised data structures. At the top level,
it is a series of equal-size records, one per target data block.

The number of target data blocks is `CEIL(length / blocksize)` from the
corresponding headers. block-checksum data is stored serially in the checksum
section in the same order as the data blocks in the target file.

The record for each data block consists of some octets of data containing the
rolling checksums of the corresponding target data block, followed immediately
by some octets of data containing the strong hash of the same. The length of
each block-checksums is, as will be seen below, `rsum_length` +
`strong_hash_length` octets.

#### Rolling checksum

A rolling checksum is an adler32-like checksum of a data block. It consists of two 16-bit integers:

```
A = sum(d_0 + d_1 + d_2 + ... + d_blocksize-1)   (mod 65536)
B = sum(blocksize*d_0 + (blocksize-1)*d_1 + ... + d_blocksize-1)   (mod 65536)
```

For short data blocks (the last block of a target file often has fewer than
blocksize bytes), the target data is passed with NULs up to the blocksize.

The representation of the rolling checksum depends on the Hash-Lengths header.

##### rsum_length = 4

```
rolling-checksum = A B
```

where A, B are represented as big-endian binary 16-bit integers.

##### rsum_length = 3

```
rolling_checksum = A_8 B
```

where A_8 = A & 0xff represented as a single binary octet, and B is a big-endian binary 16-bit integer.

##### rsum_length = 2

```
rolling_checksum = A_8 B_8
```

where A_8 = A & 0xff and B_8 = B & 0xff, each encoded as a single binary octet.

#### Strong Hash

The `strong-hash` is the first `strong_hash_length` bytes of the binary
representation of the digest of the target data block using the digest
algorithm specified by the `Strong-Hash-Algorithm` header field.

For short data blocks (the last block of a target file often has fewer than
blocksize bytes), the target data is passed with NULs up to the blocksize.

##  Security Considerations

zsync's protocol is not intended in general to provide security guarantees. The
inclusion of a whole-file checksum in the zsync control file is similar to
including a `<name>.sha1` or `CHECKSUMS` file alongside a download; since the
server operator control both the download and the checksum file alongside it, a
compromise of the server or malicious operator can trivially modify both the
file and the checksum recorded alongside it. A user either trusts the download
server, or should look for an upstream checksum or signature for the target
file to verify it.

In one case, zsync provides a security guarantee of sorts. If an absolute URL
is provided in the zsync control file pointing to a mirror of the target data,
then the checksum in the control file does provide a cross-check that the
resulting download matches not the hosted target file data, but the *expected*
target file data. That matters in particular because the use of a third-party
mirror server is often transparent to the user: even if a zsync client prints
the URL being used for target data download, the user is unlikely to read it if
the download works. Therefore if the reconstructed target file does not match an
included File-Hash, the zsync client SHOULD return an error to the user and
SHOULD NOT put the target file in place with its final filename — a mismatched
digest indicates at least a corrupted file, but could indicated an
intentionally tampered download.

zsync is potentially vulnerable to a DoS via data poisoning in the download
file itself. Linux distribution ISOs are the most common use case for zsync,
and it is not necessarily that hard to inject data into Linux distribution ISOs
- zsync itself is an example of a program included in some Linux ISOs. A
malicious actor could make an file or image that legitimately gets into Linux
ISOs but which contains hash collisions for data blocks. That would poison the
file for zsync. To mitigate this:

- zsync generators SHOULD use `strong_hash_length` equal to the full
  length of the selected strong hash digest;
- zsync generators SHOULD include a File-Hash header.
- zsync clients SHOULD calculate the corresponding digest of the reconstructed
  target file after reconstruction is complete, and compare with the digest does
  from the File-Hash header.
  - zsync client MAY want to support fallback to downloading the full target
    file if verification fails for automated use cases.
- applications in automated uses cases SHOULD NOT use MD4 for zsync block
  checksums.

## Author & Acknowledgements

Author: Colin Phipps <cph@moria.org.uk>

Some of the revisions to the original zsync control file format used here [were
first proposed for
zsync2](https://go-deltasync.github.io/docs/latest/zsync2/proposal-blake3/) by
[tannevaled](https://github.com/tannevaled). Their reverse engineering of the
format from the code of zsync 0.6 also prompted me to write this specification.

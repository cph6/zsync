/*
 *   zsync/lib - library for using the rsync algorithm to determine
 *               which parts of a file you have and which you need.
 *   Copyright (C) 2004 Colin Phipps <cph@moria.org.uk>
 *
 *   This program is free software; you can redistribute it and/or modify
 *   it under the terms of the Artistic License v2 (see the accompanying 
 *   file COPYING for the full license terms), or, at your option, any later 
 *   version of the same license.
 *
 *   This program is distributed in the hope that it will be useful,
 *   but WITHOUT ANY WARRANTY; without even the implied warranty of
 *   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *   COPYING file for details.
 */

/* This is the library interface. Very changeable at this stage. */

#include <stdio.h>

struct zsync_state;

typedef int zs_blockid;

struct rsum {
	unsigned short	a;
	unsigned short	b;
} __attribute__((packed));

#define CHECKSUM_SIZE 16

struct zsync_state* zsync_init(zs_blockid nblocks, size_t blocksize);
void zsync_end(struct zsync_state* z);

/* These transfer out the filename and handle of the file backing the data retrieved.
 * Once you have transferred out the file handle, you can no longer read and write data through libzsync - it has handed it over to you, and can use it no more itself.
 * If you transfer out the filename, you are responsible for renaming it to something useful. If you don't transfer out the filename, libzsync will unlink it at zsync_end.
 */
char* zsync_filename(struct zsync_state* z);
int zsync_filehandle(struct zsync_state* z);

void add_target_block(struct zsync_state* z, zs_blockid b, struct rsum r, void* checksum);

int submit_blocks(struct zsync_state* z, unsigned char* data, zs_blockid bfrom, zs_blockid bto);
int submit_source_data(struct zsync_state* z, unsigned char* data, size_t len, long long offset);
int submit_source_file(struct zsync_state* z, FILE* f);

/* This reads back in data which is already known. */
int read_known_data(struct zsync_state* z, unsigned char* buf, long long offset, size_t len);

/* get_needed_block_ranges tells you what blocks, within the given range,
 * are still unknown. It returns a list of block ranges in r[]
 * (at most max ranges, so spece for 2*max elements must be there)
 * these are half-open ranges, so r[0] <= x < r[1], r[2] <= x < r[3] etc are needed */
int get_needed_block_ranges(struct zsync_state* z, zs_blockid* r, int max, zs_blockid from, zs_blockid to);

/* For preparing zsync control files - in both cases len is the block size. */
struct rsum __attribute__((pure)) calc_rsum_block(const unsigned char* data, size_t len);
void calc_checksum(unsigned char *c, const unsigned char* data, size_t len);

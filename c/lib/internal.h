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

/* Internal data structures to the library. Not to be included by code outside libzsync. */

struct hash_entry {
  struct hash_entry* next;
  struct rsum r;
  unsigned char checksum[CHECKSUM_SIZE];
};

struct zsync_state {
  struct rsum r[2]; /* Current rsums */

  zs_blockid blocks;
  size_t blocksize;
  int blockshift;
  unsigned short rsum_a_mask;
  int checksum_bytes;
  int seq_matches;

  int context; /* precalculated blocksize * seq_matches */

  /* These are used by the library. Note, not thread safe. */
  const struct hash_entry* rover;
  const struct hash_entry* next_match;
  int skip; /* skip forward on next submit_source_data */

  /* Hash table for rsync algorithm */
  int hashmask;
  struct hash_entry* blockhashes;
  struct hash_entry** rsum_hash;

  /* Current state and stats for data collected by algorithm */
  int numranges;
  zs_blockid* ranges;
  int gotblocks;
  struct {
    int hashhit, weakhit, stronghit, checksummed;
  } stats;

  /* Temp file for output */
  char* filename;
  int fd;
};

static inline zs_blockid get_HE_blockid(const struct zsync_state* z, const struct hash_entry* e) { return e - z->blockhashes; }

void add_to_ranges(struct zsync_state* z, zs_blockid n);
int already_got_block(struct zsync_state* z, zs_blockid n);

struct hash_entry* calc_hash_entry(void* data, size_t len);

static inline unsigned calc_rhash(const struct zsync_state* const z, const struct hash_entry* const e) {
  unsigned h = e[0].r.b;
  
  if (z->seq_matches > 1)
    h ^= e[1].r.b;
  
  /* Mask for array */
  return h & z->hashmask;
}

void build_hash(struct zsync_state* z);

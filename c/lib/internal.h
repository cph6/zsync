/*
 *   zsync/lib - library for using the rsync algorithm to determine
 *               which parts of a file you have and which you need.
 *   Copyright (C) 2004 Colin Phipps <cph@moria.org.uk>
 *
 *   This program is free software; you can redistribute it and/or modify
 *   it under the terms of the GNU General Public License as published by
 *   the Free Software Foundation; either version 2 of the License, or
 *   (at your option) any later version.
 *
 *   This program is distributed in the hope that it will be useful,
 *   but WITHOUT ANY WARRANTY; without even the implied warranty of
 *   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *   GNU General Public License for more details.

 *   You should have received a copy of the GNU General Public License
 *   along with this program; if not, write to the Free Software
 *   Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA  02111-1307  USA
 */

/* Internal data structures to the library. Not to be included by code outside libzsync. */

struct hash_entry {
  struct hash_entry* next;
  struct rsum r;
  unsigned char checksum[CHECKSUM_SIZE];
};

struct zsync_state {
  zs_blockid blocks;
  size_t blocksize;
  int blockshift;

  int hashmask;
  struct hash_entry* blockhashes;
  struct hash_entry** rsum_hash;

  struct rsum current_rsum;
  int skip; /* skip forward on next submit_source_data */

  int gotblocks;
  int numranges;
  zs_blockid* ranges;

  char* filename;
  int fd;
};

static inline zs_blockid get_HE_blockid(const struct zsync_state* z, const struct hash_entry* e) { return e - z->blockhashes; }

void add_to_ranges(struct zsync_state* z, zs_blockid n);
int already_got_block(struct zsync_state* z, zs_blockid n);

struct hash_entry* calc_hash_entry(void* data, size_t len);

static inline int calc_rhash(const struct zsync_state* z, struct rsum r) { return r.b & z->hashmask; }

static inline const struct hash_entry* __attribute__((pure)) get_first_hash_entry(const struct zsync_state* z, struct rsum r) {
  return z->rsum_hash[calc_rhash(z, r)];
}



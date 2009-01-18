/*
 *   rcksum/lib - library for using the rsync algorithm to determine
 *               which parts of a file you have and which you need.
 *   Copyright (C) 2004,2005,2007,2009 Colin Phipps <cph@moria.org.uk>
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

#include "zsglobal.h"

#include <stdlib.h>
#include <string.h>
#include <sys/types.h>

#ifdef WITH_DMALLOC
# include <dmalloc.h>
#endif

#include "rcksum.h"
#include "internal.h"

void rcksum_add_target_block(struct rcksum_state* z, zs_blockid b, struct rsum r, void* checksum)
{
 if (b < z->blocks) {
  /* Get hash entry with checksums for this block */
  struct hash_entry* e = &(z->blockhashes[b]);

  /* Enter checksums */
  memcpy(e->checksum, checksum, z->checksum_bytes);
  e->r.a = r.a & z->rsum_a_mask;
  e->r.b = r.b;
  if (z->rsum_hash) {
    free(z->rsum_hash); z->rsum_hash = NULL;
    free(z->bithash); z->bithash = NULL;
  }
 }
}

int build_hash(struct rcksum_state* z)
{
  zs_blockid id;
  int i = 16;
  
  while ((2<<(i-1)) > z->blocks && i > 4) i--;
  z->bithashmask = (2<<(i+BITHASHBITS)) - 1;
  z->hashmask = (2<<i) - 1;

  z->rsum_hash = calloc(z->hashmask+1, sizeof *(z->rsum_hash));
  if (!z->rsum_hash) return 0;

  z->bithash = calloc(z->bithashmask+1, 1);
  if (!z->bithash) { free(z->rsum_hash); z->rsum_hash = NULL; return 0; }

  for (id = 0; id < z->blocks; id++) {
    struct hash_entry* e = z->blockhashes + id;
    /* Prepend to linked list for this hash entry */
    unsigned h = calc_rhash(z,e);

    e->next = z->rsum_hash[h & z->hashmask];
    z->rsum_hash[h & z->hashmask] = e;

    z->bithash[(h & z->bithashmask) >> 3] |= 1 << (h & 7);
  }
  return 1;
}


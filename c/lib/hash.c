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

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "zsync.h"
#include "internal.h"

void add_target_block(struct zsync_state* z, zs_blockid b, struct rsum r, void* checksum)
{
 if (b < z->blocks) {
  /* Get hash entry with checksums for this block */
  struct hash_entry* e = &(z->blockhashes[b]);
  int h;

  /* Enter checksums */
  memcpy(e->checksum, checksum, CHECKSUM_SIZE);
  e->r = r;

  /* Prepend to linked list for this hash entry */
  h = calc_rhash(z, r);
  e->next = z->rsum_hash[h];
  z->rsum_hash[h] = e;
 }
}


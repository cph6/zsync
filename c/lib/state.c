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

/* Effectively the constructor and destructor for the zsync object.
 * Also handles the file handles on the temporary stote.
 */

#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "zsync.h"
#include "internal.h"

struct zsync_state* zsync_init(zs_blockid nblocks, size_t blocksize, int rsum_bytes, int checksum_bytes, int require_consecutive_matches)
{
  struct zsync_state* z = malloc(sizeof(struct zsync_state));

  if (z != NULL) {
    /* Setup blocksize and shift. Size must be a power of two. */
    z->blocksize = blocksize;
    z->blocks = nblocks;
    z->rsum_a_mask = rsum_bytes < 3 ? 0 : rsum_bytes == 3 ? 0xff : 0xffff;
    z->checksum_bytes = checksum_bytes;
    z->seq_matches = require_consecutive_matches;
    z->context = blocksize * require_consecutive_matches;
    z->gotblocks = 0;
    z->filename = strdup("zsync-XXXXXX");
    memset(&(z->stats),0,sizeof(z->stats));
    if (!(z->blocksize & (z->blocksize - 1)) && z->filename != NULL && z->blocks) {
      z->fd = mkstemp(z->filename);
      if (z->fd == -1) {
	perror("open");
      } else {
	{
	  int i;
	  for (i = 0; i < 32; i++)
	    if (z->blocksize == (1 << i)) {
	      z->blockshift = i; break;
	    }
	}
	
	z->ranges = NULL;
	z->rsum_hash = NULL;
	z->numranges = 0;
	  
	z->blockhashes = malloc(sizeof(z->blockhashes[0]) * (z->blocks+z->seq_matches));
	if (z->blockhashes != NULL)
	  return z;

	/* All below is error handling */
      }
    }
    free(z->filename);
    free(z);
  }
  return NULL;
}

char* zsync_filename(struct zsync_state* z)
{
  char* p = z->filename;
  z->filename = NULL;
  return p;
}

int zsync_filehandle(struct zsync_state* z)
{
  int h = z->fd;
  z->fd = -1;
  return h;
}

void zsync_end(struct zsync_state* z) 
{
  if (z->fd != -1)
    close(z->fd);
  if (z->filename) {
    unlink(z->filename);
    free(z->filename);
  }
  if (z->rsum_hash) free(z->rsum_hash);
  free(z->ranges); // Should be NULL already
  fprintf(stderr,"hashhit %d, weakhit %d, checksummed %d, stronghit %d\n",z->stats.hashhit, z->stats.weakhit, z->stats.checksummed, z->stats.stronghit);
  free(z);
}


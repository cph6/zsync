/*
 *   rcksum/lib - library for using the rsync algorithm to determine
 *               which parts of a file you have and which you need.
 *   Copyright (C) 2004,2005 Colin Phipps <cph@moria.org.uk>
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

/* Effectively the constructor and destructor for the rcksum object.
 * Also handles the file handles on the temporary stote.
 */

#include "config.h"

#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "rcksum.h"
#include "internal.h"

struct rcksum_state* rcksum_init(zs_blockid nblocks, size_t blocksize, int rsum_bytes, int checksum_bytes, int require_consecutive_matches)
{
  struct rcksum_state* z = malloc(sizeof(struct rcksum_state));

  if (z != NULL) {
    /* Setup blocksize and shift. Size must be a power of two. */
    z->blocksize = blocksize;
    z->blocks = nblocks;
    z->rsum_a_mask = rsum_bytes < 3 ? 0 : rsum_bytes == 3 ? 0xff : 0xffff;
    z->checksum_bytes = checksum_bytes;
    z->seq_matches = require_consecutive_matches;
    z->context = blocksize * require_consecutive_matches;
    z->gotblocks = 0;
    z->filename = strdup("rcksum-XXXXXX");
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

char* rcksum_filename(struct rcksum_state* z)
{
  char* p = z->filename;
  z->filename = NULL;
  return p;
}

int rcksum_filehandle(struct rcksum_state* z)
{
  int h = z->fd;
  z->fd = -1;
  return h;
}

void rcksum_end(struct rcksum_state* z) 
{
  if (z->fd != -1)
    close(z->fd);
  if (z->filename) {
    unlink(z->filename);
    free(z->filename);
  }
  free(z->rsum_hash);
  free(z->blockhashes);
  free(z->bithash);
  free(z->ranges); // Should be NULL already
#ifdef DEBUG
  fprintf(stderr,"hashhit %d, weakhit %d, checksummed %d, stronghit %d\n",z->stats.hashhit, z->stats.weakhit, z->stats.checksummed, z->stats.stronghit);
#endif
  free(z);
}


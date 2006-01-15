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

/* Effectively the constructor and destructor for the zsync object.
 * Also handles the file handles on the temporary stote.
 */

#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "zsync.h"
#include "internal.h"

struct zsync_state* zsync_init(zs_blockid nblocks, size_t blocksize)
{
  struct zsync_state* z = malloc(sizeof(struct zsync_state));

  if (z != NULL){
    /* Setup blocksize and shift. Size must be a power of two. */
    z->blocksize = blocksize;
    z->blocks = nblocks;
    z->gotblocks = 0;
    z->filename = strdup("zsync-XXXXXX");
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
	
	z->hashmask = 0xffff;
	z->rsum_hash = calloc(z->hashmask+1, sizeof *(z->rsum_hash));
	if (z->rsum_hash != NULL) {
	  z->ranges = NULL;
	  z->numranges = 0;
	  
	  z->blockhashes = malloc(sizeof(z->blockhashes[0]) * z->blocks);
	  if (z->blockhashes != NULL)
	    return z;

	  /* All below is error handling */
	  free(z->rsum_hash);
	}
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
  free(z->rsum_hash);
  free(z->ranges); // Should be NULL already
  free(z);
}


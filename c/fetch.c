/*
 *   zsync - client side rsync over http
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

#include <stdlib.h>
#include <string.h>

#include "zsync.h"
#include "fetch.h"
#include "http.h"

int fetch_remaining_blocks_http(struct zsync_state* z, const char* url, int maxblocks)
{
  struct range_fetch* rf = range_fetch_start(url);
  int nrange;
  int lowwatermark = 0;
  int ret = 0;
  int lastmegsdown = 0;

  if (!rf) return -1;
  fprintf(stderr,"downloading from %s:",url);

  do {
    {
#define MAXRANGES 100
      zs_blockid blrange[MAXRANGES*2];
      long long byterange[MAXRANGES*2];
      int i,j;

      nrange = get_needed_block_ranges(z, &blrange[0], MAXRANGES, lowwatermark, 0x7fffffff);
      for (i=j=0; i<nrange; i++) {
	lowwatermark = blrange[2*i+1]; /* Don't queue these again */
	if (maxblocks && blrange[2*i+1] - blrange[2*i] > maxblocks) continue;
	byterange[2*j] = blrange[2*i]*blocksize;
	byterange[2*j+1] = blrange[2*i+1]*blocksize-1;
	j++;
      }
      nrange = j;

      if (nrange)
        range_fetch_addranges(rf, byterange, nrange);
    }
    {
      int len;
      unsigned char* buf;
      long long offset;
      
      buf = malloc(4*blocksize);
      if (!buf) { http_down += range_fetch_bytes_down(rf); range_fetch_end(rf); return -1; }

      while ((len = get_range_block(rf, &offset, buf, 4*blocksize)) > 0) {
	if (offset % blocksize == 0) {
	  if (len % blocksize != 0) {
	    /* We get a truncated final block for the last part of the file stream. Let's assume it is due to that, and pad with zeroes to the block size. */
	    int extra = blocksize - len % blocksize;
	    memset(&buf[len],0,extra); len += extra;
	  }
	  ret |= submit_blocks(z, buf, offset/blocksize, (offset+len-1)/blocksize);
	} else
	  fprintf(stderr,"got misaligned data? %lld\n",offset);

	if (nrange) break;
	{
	  int md = range_fetch_bytes_down(rf)/1000000;
	  if (md != lastmegsdown) {
	    lastmegsdown = md; fputc('.',stderr);
	  }
	}
      }
      if (len == -1) ret = -1;
      free(buf);
    }
  } while (!ret && nrange);
  http_down += range_fetch_bytes_down(rf);
  range_fetch_end(rf);
  fputc('\n',stderr);
  return ret;
}


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
#include "http.h"
#include "fetch.h"

#include "zlib/zlib.h"

int fetch_remaining_blocks_zlib_http(struct zsync_state* z, const char* url, struct gzblock* zblock, int nzblocks)
{
#define MAXRANGES 100
    long long zbyterange[MAXRANGES*2];
    long long rangestart[MAXRANGES];
    unsigned int zdiscardbits[MAXRANGES];
    int nrange;
    int ret = 0;

    {
      int i;
      zs_blockid blrange[MAXRANGES*2];

      nrange = get_needed_block_ranges(z, &blrange[0], MAXRANGES, 0, 0x7fffffff);
      if (nrange == 0) return 0;

      /* Translate into byte blocks to retrieve from the gzip file */
      for (i=0; i<nrange; i++) {
	long long start = blocksize*(long long)blrange[2*i];
	long long end = blocksize*(long long)blrange[2*i+1];
	long long zstart = -1;
	long long zend = -1;
	long long outstart = -1;
	long long inbitoffset = 0;
	long long outbyteoffset = 0;
	long long lastblockstart_inbitoffset = 0;
	long long lastblockstart_outbyteoffset = 0;
	int j;
	
	for (j=0; j<nzblocks && (zstart == -1 || zend == -1); j++) {
	  inbitoffset += zblock[j].inbitoffset & 0x7fffffff;
	  outbyteoffset += zblock[j].outbyteoffset;

	  /* Is this the first block that comes after the start point - if so, the previous block header is the place to start (hence we don't update the lastblockstart_* values until after this) */
	  if (start < outbyteoffset && zstart == -1) {
	    if (j == 0) break;
	    zstart = lastblockstart_inbitoffset;
	    outstart = lastblockstart_outbyteoffset;
	    // fprintf(stderr,"starting range at zlib block %d offset %lld.%lld\n",j-1,zstart/8,zstart%8);
	  }
	  
	  if (!(zblock[j].inbitoffset & 0x80000000)) { /* Block starts here */
	    lastblockstart_inbitoffset = inbitoffset;
	    lastblockstart_outbyteoffset = outbyteoffset;
	  }

	  /* If we have passed the start, and we have now passed the end, then the end of this block is the end of the range to fetch. Special case end of stream, where the range libzsync knows about could extend beyond the range of the zlib stream. */
	  if (start < outbyteoffset && (end <= outbyteoffset || j == nzblocks-1)) {
	    zend = inbitoffset;
	    // fprintf(stderr,"ending range at zlib block %d offset %lld.%lld\n",j,zend/8,zend%8);
	  }
	}
	if (zend == -1 || zstart == -1) {
	  fprintf(stderr,"Z-Map couldn't tell us how to find blocks %d-%d\n",blrange[2*i],blrange[2*i+1]);
	  return -1;
	}
	zbyterange[2*i] = zstart/8;
	/* Note +9 trailing bits, the offset is to the last huffman code inside the block, and each code is at most 9 bits */
	zbyterange[2*i+1] = (zend+7)/8;
	rangestart[i] = outstart;
	zdiscardbits[i] = zstart%8;
      }
      for (i=0; i<nrange-1;) {
	if (zbyterange[2*i+1] >= zbyterange[2*(i+1)]) {
	  // Ranges overlap, merge
	  // fprintf(stderr,"merging ranges %lld-%lld and %lld-%lld\n",zbyterange[2*i+0],zbyterange[2*i+1],zbyterange[2*i+2],zbyterange[2*i+3]);
	  /* Copy the end of the seond range over the end of the first (so the new range covers both); and copy down the rest of the array - both done in one memmove, hence the odd (2*(number fo topy down)+1) */
	  memmove(&zbyterange[2*i+1],&zbyterange[2*i+3], (2*(nrange-2-i)+1)*sizeof(zbyterange[0]));
	  if (i < nrange-2) {
	    memmove(&rangestart[i+1],&rangestart[i+2],(nrange-2-i)*sizeof(rangestart[0]));
	    memmove(&zdiscardbits[i+1],&zdiscardbits[i+2],(nrange-2-i)*sizeof(zdiscardbits[0]));
	  }
	  nrange--;
	} else i++;
      }
    }
    {
      struct range_fetch* rf = range_fetch_start(url);
      int len;
      unsigned char* buf;
      unsigned char* obuf;
      unsigned char* wbuf;
      long long zoffset;
      int cur_range = -1;
      z_stream zs;
      int eoz = 0;
      long outoffset = -1;
      
      if (!rf) return -1;
      range_fetch_addranges(rf, zbyterange, nrange);

      buf = malloc(4*blocksize);
      if (!buf) { http_down += range_fetch_bytes_down(rf); range_fetch_end(rf); return -1; }
      obuf = malloc(blocksize);
      if (!obuf) { free(buf); http_down += range_fetch_bytes_down(rf); range_fetch_end(rf); return -1; }
      wbuf = malloc(32768);
      if (!wbuf) { free(obuf); free(buf); http_down += range_fetch_bytes_down(rf); range_fetch_end(rf); return -1; }

      while ((len = get_range_block(rf, &zoffset, buf, 4*blocksize)) > 0) {

	/* Load into the zlib input buffer. If this is the start of a new block, (re)initialise the inflate system. */
	if (zoffset == zbyterange[2*(cur_range+1)]) {
	  /* Release any old inflate object */
	  if (cur_range++ != -1) inflateEnd(&zs);

	  /* Work out what the decompressed data will correspond to */
	  outoffset = rangestart[cur_range];

	  /* Set up new inflate object */
	  zs.zalloc = Z_NULL; zs.zfree = Z_NULL; zs.opaque = NULL;
	  inflateInit2(&zs,-MAX_WBITS);

	  /* Now set up for the downloaded block */
	  zs.next_in = buf; zs.avail_in = len;
	  /* Align with the bitstream */
	  inflate_advance_bits(&zs, zdiscardbits[cur_range]);

	  { /* Load in prev 32k sliding window for backreferences */
	    long long pos = outoffset;
	    int lookback = (pos > 32768) ? 32768 : pos;

	    read_known_data(z, wbuf, pos-lookback,lookback);
	    /* Fake an output buffer of 32k filled with data to zlib */
	    zs.next_out = wbuf+lookback; zs.avail_out = 0;
	    updatewindow(&zs,lookback);
	  }
	  
	  /* On first iteration, we might be reading an incomplete block from zsync's point of view. Limit avail_out so we can stop after doing that and realign with the buffer. */
	  zs.avail_out = blocksize - (outoffset % blocksize);
	  zs.next_out = obuf;
	} else {
	  if (outoffset == -1) { fprintf(stderr,"data didn't align with block boundary in compressed stream\n"); break; }
	  zs.next_in = buf; zs.avail_in = len;
	}
	
	while (zs.avail_in && !eoz) {
	  int rc;
	  
	  /* Read in up to the next block (in the libzsync sense on the output stream) boundary */

	  rc = inflate(&zs,Z_SYNC_FLUSH);
	  switch (rc) {
	  case Z_STREAM_END: eoz = 1;
	  case Z_OK:
	    if (zs.avail_out == 0 || eoz) {
	      /* If this was at the start of a block, try submitting it */
	      if (!(outoffset % blocksize)) {
		zs_blockid cur_block = outoffset / blocksize;
		if (zs.avail_out) memset(zs.next_out,0,zs.avail_out);
		ret |= submit_blocks(z, obuf, cur_block, cur_block);
		outoffset += blocksize;
	      } else {
		/* We were reading a block fragment; update outoffset, and we are nwo block-aligned. */
		outoffset += zs.next_out - obuf;
	      }
	      zs.avail_out = blocksize; zs.next_out = obuf;
	    }
	    break;
	  default:
	    fprintf(stderr,"zlib error: %s\n",zs.msg);
	    eoz=1; ret = -1; break;
	  }
	}
      }
      if (len < 0) ret = -1;
      if (outoffset != -1) { inflateEnd(&zs); }
      free(wbuf);
      free(obuf);
      free(buf);
      http_down += range_fetch_bytes_down(rf);
      range_fetch_end(rf);
    }
    return ret;
}


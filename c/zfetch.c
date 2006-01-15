/*
 *   zsync - client side rsync over http
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

#include <stdlib.h>
#include <string.h>

#include "zsync.h"
#include "libzmap/zmap.h"

#include "http.h"
#include "fetch.h"

#include "zlib/zlib.h"

int fetch_remaining_blocks_zlib_http(struct zsync_state* z, const char* url, const struct zmap* zm)
{
#define MAXRANGES 100
  long long byterange[MAXRANGES*2];
  int nrange;
  int ret = 0;
  
  {
    int i;
    zs_blockid blrange[MAXRANGES*2];
    
    nrange = get_needed_block_ranges(z, &blrange[0], MAXRANGES, 0, 0x7fffffff);
    if (nrange == 0) return 0;
    
    for (i=0; i<nrange; i++) {
      byterange[2*i] = blrange[2*i]*blocksize;
      byterange[2*i+1] = blrange[2*i+1]*blocksize-1;
    }
  }

  {
    long long zbyterange[MAXRANGES*2];

    nrange = map_to_compressed_ranges(zm, zbyterange, MAXRANGES, byterange, nrange);
    if (nrange <= 0)
      return nrange;

    fprintf(stderr,"downloading from %s:",url);
  
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
      long long outoffset = -1;
      
      /* Set up new inflate object */
      zs.zalloc = Z_NULL; zs.zfree = Z_NULL; zs.opaque = NULL;
      zs.total_in = 0;

      if (!rf) return -1;
      range_fetch_addranges(rf, zbyterange, nrange);

      buf = malloc(4*blocksize);
      if (!buf) { http_down += range_fetch_bytes_down(rf); range_fetch_end(rf); return -1; }
      obuf = malloc(blocksize);
      if (!obuf) { free(buf); http_down += range_fetch_bytes_down(rf); range_fetch_end(rf); return -1; }
      wbuf = malloc(32768);
      if (!wbuf) { free(obuf); free(buf); http_down += range_fetch_bytes_down(rf); range_fetch_end(rf); return -1; }

      while ((len = get_range_block(rf, &zoffset, buf, 4*blocksize)) > 0) {
	/* Now set up for the downloaded block */
	zs.next_in = buf; zs.avail_in = len;

	if (zs.total_in == 0 || zoffset != zs.total_in) {
	  cur_range++;
	  configure_zstream_for_zdata(zm, &zs, zoffset, &outoffset);

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
		int rc;
		zs_blockid cur_block = outoffset / blocksize;
		if (zs.avail_out) memset(zs.next_out,0,zs.avail_out);
		rc = submit_blocks(z, obuf, cur_block, cur_block);
		if (!zs.avail_out) ret |= rc;
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
      if (zs.total_in > 0) { inflateEnd(&zs); }
      free(wbuf);
      free(obuf);
      free(buf);
      http_down += range_fetch_bytes_down(rf);
      range_fetch_end(rf);
    }
    fputc('\n',stderr);
    return ret;
  }
}


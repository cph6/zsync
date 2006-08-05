/*
 *   zsync - client side rsync over http
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

#include "config.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/types.h>
#include <arpa/inet.h>
#ifdef HAVE_INTTYPES_H
#include <inttypes.h>
#endif

#include "zmap.h"

struct zmapentry {
  long long inbits;
  long long outbytes;
  int blockcount;
};

struct zmap {
  int n;
  struct zmapentry* e;
};

struct zmap* zmap_make(const struct gzblock* zb, int n)
{
  struct zmap* m = malloc(sizeof(struct zmap));

  if (!m) return m;
  
  m->n = n;
  m->e = malloc(sizeof(struct zmapentry)*n);

  if (!m->e) { free(m); return NULL; }
  
  {
    long long in = 0;
    long long out = 0;
    int i;
    int bc = 0;

    for (i=0; i<n; i++) {
      uint16_t ob = ntohs(zb[i].outbyteoffset);

      if (ob & GZB_NOTBLOCKSTART) { /* Not a zlib block start, just a mid-block mark */
	ob &= ~GZB_NOTBLOCKSTART; bc++;
      }
      else { bc = 0; }

      in += ntohs(zb[i].inbitoffset);
      out += ob;

      /* And write the entry */
      m->e[i].inbits = in;
      m->e[i].outbytes = out;
      m->e[i].blockcount = bc;
    }
  }
  return m;
}

void zmap_free(struct zmap* m)
{
  free(m->e);
  free(m);
}

/* Translate into byte blocks to retrieve from the gzip file */

off_t* zmap_to_compressed_ranges(const struct zmap* zm, off_t* byterange, int nrange, int* num)
{
  int i,k;
  long long lastwroteblockstart_inbitoffset = 0;
  int k_at_last_block = -1;
  off_t* zbyterange = malloc(2 * 2 * nrange * sizeof *byterange);

  for (i=0,k=0; i<nrange; i++) {
    long long start = byterange[2*i];
    long long end = byterange[2*i+1];
    long long zstart = -1;
    long long zend = -1;
    long long lastblockstart_inbitoffset = 0;
    int j;
    
    for (j=0; j<zm->n && (zstart == -1 || zend == -1); j++) {
      register long long inbitoffset = zm->e[j].inbits;
      register long long outbyteoffset = zm->e[j].outbytes;
      
      /* Is this the first block that comes after the start point - if so, the previous block header is the place to start (hence we don't update the lastblockstart_* values until after this) */
      if (start < outbyteoffset && zstart == -1) {
	if (j == 0) break;
	zstart = zm->e[j-1].inbits;
	// fprintf(stderr,"starting range at zlib block %d offset %lld.%lld\n",j-1,zstart/8,zstart%8);
	if (lastwroteblockstart_inbitoffset != lastblockstart_inbitoffset) {
	  /* The zbyteranges 0...k-1 have retrieved everything we need up to the start of the previous block - record this fact for the caller */
	  k_at_last_block = k;

	  zbyterange[2*k] = lastblockstart_inbitoffset/8;
	  zbyterange[2*k+1] = (lastblockstart_inbitoffset/8) + 200;
	  k++;
	  lastwroteblockstart_inbitoffset = lastblockstart_inbitoffset;
	}
      }
      
      if (!(zm->e[j].blockcount)) { /* Block starts here */
	lastblockstart_inbitoffset = inbitoffset;
      }

      /* If we have passed the start, and we have now passed the end, then the end of this block is the end of the range to fetch. Special case end of stream, where the range libzsync knows about could extend beyond the range of the zlib stream. */
      if (start < outbyteoffset && (end <= outbyteoffset || j == zm->n - 1)) {
	zend = inbitoffset;
	// fprintf(stderr,"ending range at zlib block %d offset %lld.%lld\n",j,zend/8,zend%8);
      }
    }
    if (zend == -1 || zstart == -1) {
      fprintf(stderr,"Z-Map couldn't tell us how to find %lld-%lld\n",byterange[2*i],byterange[2*i+1]);
      free(zbyterange);
      return NULL;
    }
    zbyterange[2*k] = zstart/8;
    /* Note +9 trailing bits, the offset is to the last huffman code inside the block, and each code is at most 9 bits */
    zbyterange[2*k+1] = (zend+7)/8;
    k++;
  }

  for (i=0; i<k-1;) {
    if (zbyterange[2*i+1] >= zbyterange[2*(i+1)]) {
      // Ranges overlap, merge
      // fprintf(stderr,"merging ranges %lld-%lld and %lld-%lld\n",zbyterange[2*i+0],zbyterange[2*i+1],zbyterange[2*i+2],zbyterange[2*i+3]);
      /* Second range might be contained in the first */
      if (zbyterange[2*i+1] < zbyterange[2*(i+1)+1])
	zbyterange[2*i+1] = zbyterange[2*(i+1)+1];
      /* Copy the rest of the array down */
      memmove(&zbyterange[2*i+2],&zbyterange[2*i+4], 2*(k-2-i)*sizeof(zbyterange[0]));
      k--;
    } else i++;
  }
  *num = k;
  if (k > 0) {
    zbyterange = realloc(zbyterange, 2 * k * sizeof *zbyterange);
  }
  return zbyterange;
}

#include "zlib/zlib.h"

void configure_zstream_for_zdata(const struct zmap* zm, z_stream* zs, long zoffset, long long* poutoffset)
{
  int i;
  { /* Binary search to find this offset in the Z-Map */
    int low = 0;
    int high = zm->n - 1;

    while (low <= high) {
      int m = (low+high)/2;
      long long inbyte = zm->e[m].inbits/8;
      if (inbyte == zoffset) {
	low = high = m;
	break;
      } else if (zoffset < inbyte) {
	high = m-1;
      } else {
	low = m+1;
      }
    }
    if (low > high) {
      fprintf(stderr,"bad offset %ld, not in z-map\n",zoffset);
      exit(3);
    }
    i = low;
  }

  if (!zm->e[i].blockcount) {
    /* Release any old inflate object */
    if (zs->total_in > 0) inflateEnd(zs);
    
    inflateInit2(zs,-MAX_WBITS);
  } else
    if (zs->total_in == 0) {
      fprintf(stderr,"bad first offset %ld, not a block start.\n",zoffset);
      exit(3);
    }

  /* Work out what the decompressed data will correspond to */
  *poutoffset = zm->e[i].outbytes;

  /* Align with the bitstream */
  inflate_advance(zs, zoffset, zm->e[i].inbits % 8, !zm->e[i].blockcount);
}

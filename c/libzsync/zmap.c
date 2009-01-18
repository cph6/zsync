/*
 *   zsync - client side rsync over http
 *   Copyright (C) 2004,2005,2007 Colin Phipps <cph@moria.org.uk>
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

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/types.h>
#include <arpa/inet.h>
#ifdef HAVE_INTTYPES_H
#include <inttypes.h>
#endif

#ifdef WITH_DMALLOC
# include <dmalloc.h>
#endif

#include "zmap.h"
#include "format_string.h"

/* This is a record of a checkpoint in a zlib stream - we have the bit position
 * (yes, bit - zlib compresses input bytes down to a variable number of bits)
 * and the corresponding output byte offset.
 * blockcount is 0 if this bit position in the zlib stream is the start of a
 *  zlib block, and is 1, 2, 3 etc for subsequent points that are in the same
 *  zlib block. */

struct zmapentry {
  long long inbits;
  long long outbytes;
  int blockcount;
};

/* Store all the zmapentry's as an array, and the # of entries */
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
  off_t* zbyterange = malloc(2 * 2 * nrange * sizeof *byterange);

  /* For each range of data that we want, work out a corresponding byte range
   * in the compressed file that definitely includes the data in the target
   * range.
   * zbyterange[0,1] is the 1st byte range, zbyterange[2,3] the second, 
   * ... and k is the number of ranges so far */

  for (i=0,k=0; i<nrange; i++) {
    /* target range in the uncompressed data */
    long long start = byterange[2*i];
    long long end = byterange[2*i+1];
    /* and the range in the compressed data which we are trying to calculate */
    long long zstart = -1;
    long long zend = -1;

    long long lastblockstart_inbitoffset = 0;
    int j;
    
    /* Step through the blocks of compressed data;
     * the zstart, zend vars are the main state for this loop:
     *  zstart == -1: we are looking for the first compressed block containing data from our target range
     *  zstart != -1, zend == -1: we found the starting block, now looking for the first compressed block that is outside the range that we need
     *  zend == -1: we got the end block too, all done
     *
     * lastblockstart_inbitoffset is a record of the offset of the most recent
     *  zlib block header - see comment in the loop to see what we need it for.
     */
    for (j=0; j<zm->n && (zstart == -1 || zend == -1); j++) {
      register long long inbitoffset = zm->e[j].inbits;
      register long long outbyteoffset = zm->e[j].outbytes;
      
      /* Is this the first block that comes after the start point - if so, the
       * previous block is the place to start */

      if (start < outbyteoffset && zstart == -1) {
        if (j == 0) break;
        zstart = zm->e[j-1].inbits;

        /* We need the zlib block header for range of compressed data
         * - you can't decompress the data without knowing the huffman tree
         * for this block of data.
         * So, immediately add a range of at least
         *  *** WARNING MAGIC NUMBER ***           200 bytes
         * (which is a guess by me, I think the zlib header never exceeds that)
         * covering the preceding zlib block header */

        if (lastwroteblockstart_inbitoffset != lastblockstart_inbitoffset) {
          zbyterange[2*k] = lastblockstart_inbitoffset/8;
          zbyterange[2*k+1] = zbyterange[2*k] + 200;
          k++;
          lastwroteblockstart_inbitoffset = lastblockstart_inbitoffset;
        }
      }
      
      /* We need to remember the most recent zlib block header, for the above.
       * (Note this is after the section above, because the code above is
       *  looking at the previous checkpoint, zm->e[j-1]. Only now do we worry
       *  about data in zm->e[j] .)
       * If blockcount == 0, this point in the compressed data is a block header */
      if (zm->e[j].blockcount == 0) { /* Block starts here */
        lastblockstart_inbitoffset = inbitoffset;
      }

      /* If we have passed the start, and we have now passed the end, then the end of this block is the end of the range to fetch. Special case end of stream, where the range libzsync knows about could extend beyond the range of the zlib stream. */
      if (start < outbyteoffset && (end <= outbyteoffset || j == zm->n - 1)) {
        zend = inbitoffset;
        /* Leave adding zstart...zend to the ranges-to-fetch to outside the loop */
      }
    }
    if (zend == -1 || zstart == -1) {
      fprintf(stderr,"Z-Map couldn't tell us how to find " OFF_T_PF "-" OFF_T_PF "\n",byterange[2*i],byterange[2*i+1]);
      free(zbyterange);
      return NULL;
    }
    
    /* Finally, translate bits to bytes and store these in our list of ranges to get */
    zbyterange[2*k] = zstart/8;
    zbyterange[2*k+1] = (zend+7)/8;
    k++;
  }

  /* The byte ranges in the compressed file can abut or overlap -
   *  so we go through and consolidate any overlapping ranges. */
  for (i=0; i<k-1;) {
    if (zbyterange[2*i+1] >= zbyterange[2*(i+1)]) {
      // Ranges overlap, merge
      /* The end of the first range need not be before the end of the
       *  second, so this if () block is to set the end of the combined block
       *  to the greater of the two.
       * The start of the second block could be before the start of the first:
       *  but this only occurs where the second range is a block header, and
       *  the first range is some data that an earlier uncompressed range
       *  needed out of the same block, in which case we are guaranteed that
       *  the block header must have been requested earlier, and so the second
       *  block here can be dropped anyway.
       */
      if (zbyterange[2*i+1] < zbyterange[2*(i+1)+1])
        zbyterange[2*i+1] = zbyterange[2*(i+1)+1];

      /* Now eliminate zbyterange[2*(i+1) +0 and +1]; */
      memmove(&zbyterange[2*i+2],&zbyterange[2*i+4], 2*(k-2-i)*sizeof(zbyterange[0]));
      k--;
    } else i++;
  }

  /* Return the # of ranges and the array of byte ranges we have built */
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

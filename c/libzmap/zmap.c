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

#include "config.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "zmap.h"

/* htons - where to get this? */
#ifdef HAVE_HTONS_IN_NETINET_IN_H
#include <netinet/in.h>
#endif
#ifdef HAVE_HTONS_IN_SYS_PARAM_H
#include <sys/param.h>
#endif

struct zmapentry {
  long long inbits;
  long long outbytes;
  int blockcount;
};

struct zmap {
  int n;
  struct zmapentry* e;
};

struct zmap* make_zmap(const struct gzblock* zb, int n)
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
      int ib = ntohl(zb[i].inbitoffset);

      if (ib & 0x80000000) { /* Not a zlib block start, just a mid-block mark */
	ib &= 0x7fffffff; bc++;
      }
      else { bc = 0; }

      in += ib;
      out += ntohl(zb[i].outbyteoffset);

      /* And write the entry */
      m->e[i].inbits = in;
      m->e[i].outbytes = out;
      m->e[i].blockcount = bc;
    }
  }
  return m;
}

/* Translate into byte blocks to retrieve from the gzip file */

int map_to_compressed_ranges(const struct zmap* zm, long long* zbyterange, int maxout, long long* byterange, int nrange)
{
  int i;
  for (i=0; i<nrange; i++) {
    long long start = byterange[2*i];
    long long end = byterange[2*i+1];
    long long zstart = -1;
    long long zend = -1;
    long long outstart = -1;
    long long inbitoffset = 0;
    long long outbyteoffset = 0;
    long long lastblockstart_inbitoffset = 0;
    long long lastblockstart_outbyteoffset = 0;
    int j;
    
    for (j=0; j<zm->n && (zstart == -1 || zend == -1); j++) {
      inbitoffset = zm->e[j].inbits;
      outbyteoffset = zm->e[j].outbytes;
      
      /* Is this the first block that comes after the start point - if so, the previous block header is the place to start (hence we don't update the lastblockstart_* values until after this) */
      if (start < outbyteoffset && zstart == -1) {
	if (j == 0) break;
	zstart = lastblockstart_inbitoffset;
	outstart = lastblockstart_outbyteoffset;
	// fprintf(stderr,"starting range at zlib block %d offset %lld.%lld\n",j-1,zstart/8,zstart%8);
      }
      
      if (!(zm->e[j].blockcount)) { /* Block starts here */
	lastblockstart_inbitoffset = inbitoffset;
	lastblockstart_outbyteoffset = outbyteoffset;
      }
      
      /* If we have passed the start, and we have now passed the end, then the end of this block is the end of the range to fetch. Special case end of stream, where the range libzsync knows about could extend beyond the range of the zlib stream. */
      if (start < outbyteoffset && (end <= outbyteoffset || j == zm->n - 1)) {
	zend = inbitoffset;
	// fprintf(stderr,"ending range at zlib block %d offset %lld.%lld\n",j,zend/8,zend%8);
      }
    }
    if (zend == -1 || zstart == -1) {
      fprintf(stderr,"Z-Map couldn't tell us how to find %lld-%lld\n",byterange[2*i],byterange[2*i+1]);
      return -1;
    }
    zbyterange[2*i] = zstart/8;
    /* Note +9 trailing bits, the offset is to the last huffman code inside the block, and each code is at most 9 bits */
    zbyterange[2*i+1] = (zend+7)/8;
  }
  for (i=0; i<nrange-1;) {
    if (zbyterange[2*i+1] >= zbyterange[2*(i+1)]) {
      // Ranges overlap, merge
      // fprintf(stderr,"merging ranges %lld-%lld and %lld-%lld\n",zbyterange[2*i+0],zbyterange[2*i+1],zbyterange[2*i+2],zbyterange[2*i+3]);
      /* Copy the end of the second range over the end of the first (so the new range covers both); and copy down the rest of the array - both done in one memmove, hence the odd (2*(number fo topy down)+1) */
      memmove(&zbyterange[2*i+1],&zbyterange[2*i+3], (2*(nrange-2-i)+1)*sizeof(zbyterange[0]));
      nrange--;
    } else i++;
  }
  return nrange;
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

  /* Release any old inflate object */
  if (zs->total_in > 0) inflateEnd(zs);
  
  inflateInit2(zs,-MAX_WBITS);

  /* Work out what the decompressed data will correspond to */
  *poutoffset = zm->e[i].outbytes;
  

  /* Align with the bitstream */
  zs->total_in = zoffset; /* We are here, plus a few more bits. */
  inflate_advance_bits(zs, zm->e[i].inbits % 8);
}

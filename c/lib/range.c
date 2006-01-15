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

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "zsync.h"
#include "internal.h"

#define calc_min_max(x, min, max) \
  int min = 0, max = zs->numranges-1;         \
  for (min=0,max=zs->numranges-1; min<=max;) { \
    int y = (max+min)/2;                      \
    if (x > zs->ranges[2*y+1]) min = y+1;     \
    else if (x < zs->ranges[2*y]) max = y-1;  \
    else { min = max = y; break; }            \
  }                                           \
  (void)0
  

void add_to_ranges(struct zsync_state* zs, zs_blockid x)
{
  calc_min_max(x, min, max);

  if (min == max) {
  } else {
    zs->gotblocks++;

    if (zs->numranges && max >=0 && min < zs->numranges && zs->ranges[2*max+1] == x-1 && zs->ranges[2*min] == x+1) {
      // This block fills the gap between two areas that we have got completely. Merge the adjacent ranges
      zs->ranges[2*max+1] = zs->ranges[2*max+3];
      memmove(&zs->ranges[2*min], &zs->ranges[2*min+2], (zs->numranges - min - 1)*sizeof(zs->ranges[0])*2);
      zs->numranges--;
    } else
    if (max >= 0 && zs->numranges && zs->ranges[2*max+1] == x-1) {
      zs->ranges[2*max+1] = x;
    } else 
    if (min < zs->numranges && zs->ranges[2*min] == x+1) {
      zs->ranges[2*min] = x;
    } else {
      // New range
      zs->ranges = realloc(zs->ranges, (zs->numranges+1) * 2 * sizeof(zs->ranges[0]));
      if (min < zs->numranges)
	memmove(&zs->ranges[2*min+2],&zs->ranges[2*min], (zs->numranges - min) * 2 * sizeof(zs->ranges[0]));
      zs->ranges[2*min] = zs->ranges[2*min+1] = x;
      zs->numranges++;
    }
  }
#if 0
  {
    int i;
    for (i=0; i<zs->numranges; i++)
      fprintf(stderr,"%d-%d,",zs->ranges[i*2],zs->ranges[i*2+1]);
    fprintf(stderr," are the current ranges got\n");
  }
#endif
}

int already_got_block(struct zsync_state* zs, zs_blockid x)
{
  calc_min_max(x, min, max);

  return (min == max);
}

int get_needed_block_ranges(struct zsync_state* z, zs_blockid* r, int max, zs_blockid from, zs_blockid to) {
  int i,n;

  if (to >= z->blocks) to = z->blocks-1;
  r[0] = from; r[1] = to; n = 1;
  /* Note r[2*n-1] is the last range in our prospective list */

  for (i = 0; i<z->numranges; i++) {
    if (z->ranges[2*i] > r[2*n-1]) continue;
    if (z->ranges[2*i+1] < from) break;
    
    /* Okay, they intersect */
    if (n == 1 && z->ranges[2*i] <= from) { /* Overlaps the start of our window */
      r[0] = z->ranges[2*i+1]+1;
    } else {
      if (z->ranges[2*i+1] >= r[2*n-1]) { /* Clips the end of our window */
	r[2*n-1] = z->ranges[2*i];
      } else {
	/* In the middle of our range, split it */
	if (n < max) {
	  r[2*n] = z->ranges[2*i+1]+1;
	  r[2*n+1] = r[2*n-1];
	}
	r[2*n-1] = z->ranges[2*i];
	if (n < max) n++;
      }
    }
  }
  if (n == 1 && r[0] > r[1]) return 0;
  return n;
}


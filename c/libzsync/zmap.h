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

#include "config.h"

#ifdef HAVE_STDINT_H
#include <stdint.h>
#else
#include <sys/types.h>
#endif

struct gzblock {
  uint16_t inbitoffset;
  uint16_t outbyteoffset;
} __attribute__((packed));

#define GZB_NOTBLOCKSTART 0x8000

struct zmap;
struct z_stream_s;

struct zmap* make_zmap(const struct gzblock* zb, int n);
off64_t* zmap_to_compressed_ranges(const struct zmap* zm, off64_t* byterange, int nrange, int* num);
void configure_zstream_for_zdata(const struct zmap* zm, struct z_stream_s* zs, long zoffset, long long* poutoffset);

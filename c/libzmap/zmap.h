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

struct gzblock {
  long inbitoffset;
  long outbyteoffset;
};

struct zmap;
struct z_stream_s;

struct zmap* make_zmap(const struct gzblock* zb, int n);
int map_to_compressed_ranges(const struct zmap* zm, long long* zbyterange, int maxout, long long* byterange, int nrange);
void configure_zstream_for_zdata(const struct zmap* zm, struct z_stream_s* zs, long zoffset, long long* poutoffset);

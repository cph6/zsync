/*
 *   zsync - client side rsync over http
 *   Copyright (C) 2004,2005,2009 Colin Phipps <cph@moria.org.uk>
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

struct progress {
  time_t starttime;
  time_t lasttime;
  float lastpcnt;
  long long lastdl;
};

extern int no_progress;

/* struct progress* = start_progress()
 * Returns a progress structure. Caller is responsible for calling
 * end_progress() on it later (which will free the memory that it uses).
 */
struct progress* start_progress(void) __attribute__((malloc));

void do_progress(struct progress* p, float pcnt, long long newdl);

/* end_progress(struct progress*, done)
 * done parameter is 0 for error, 1 for okay-but-incomplete, 2 for completed
 * This frees the memory allocated for the progress data; the pointer is no
 * longer valid when this function returns.
 */
void end_progress(struct progress* p, int done);

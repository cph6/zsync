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
#include <time.h>

#include "progress.h"

int no_progress;

static void progbar(int j, float pcnt) {
  int i;
  char buf[21];

  for (i=0; i<j && i<20; i++) buf[i] = '#';
  for (; i<20; i++) buf[i] = '-';
  buf[i] = 0;
  printf("\r%s %.1f%%",buf,pcnt);
}

void do_progress(struct progress* p, float pcnt, long long newdl)
{
  time_t newtime = time(NULL);
  if (p->lasttime != newtime) {
    int passed = p->lasttime ? newtime - p->lasttime : 0;

    if (!p->lasttime) p->starttime = newtime;

    progbar(pcnt*(20.0/100.0), pcnt);
    p->lasttime = newtime;
    if (passed) {
      float rate = newdl - p->lastdl;
      int sleft = (100.0f - pcnt) / (pcnt - p->lastpcnt);
      if (passed != 1) { rate /= passed; sleft *= passed; }
      printf(" %.1f kBps ",rate/1000.0);
      if (sleft < 60*1000)
	printf("%d:%02d ETA  ",sleft/60,sleft % 60);
      else
	puts("        ");
    }
    p->lastdl = newdl;
    p->lastpcnt = pcnt;
    fflush(stdout);
  }
}

void end_progress(struct progress* p, int done)
{
  if (done == 2) progbar(20, 100.0);
  else progbar(p->lastpcnt*(20.0/100.0), p->lastpcnt);
  
  {
    float rate = ((float)p->lastdl) / (p->lasttime - p->starttime + 0.5);
    printf(" %.1f kBps ",rate/1000.0);
  }
  puts(done == 2 ? "DONE    \n" : !done ? "aborted    \n" : "        \n");
  fflush(stdout);
}

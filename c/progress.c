
/*
 *   zsync - client side rsync over http
 *   Copyright (C) 2004,2005,2007,2009 Colin Phipps <cph@moria.org.uk>
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
#include <string.h>
#include <time.h>
#include <stdlib.h>

#ifdef WITH_DMALLOC
# include <dmalloc.h>
#endif

#include "progress.h"

#define HISTORY 10
struct progress {
  time_t starttime;
  struct {
    time_t hist_time;
    long long dl;
    float pcnt;
  } history[HISTORY];
  int num_history;
};

int no_progress;

/* progbar(chars, percent)
 * (Re)print progress bar with chars out of 20 shown and followed by the given
 * percentage done. */
static void progbar(int j, float pcnt) {
    int i;
    char buf[21];

    for (i = 0; i < j && i < 20; i++)
        buf[i] = '#';
    for (; i < 20; i++)
        buf[i] = '-';
    buf[i] = 0;
    printf("\r%s %.1f%%", buf, pcnt);
}

/* struct progress* = start_progress()
 * Returns a progress structure. Caller is responsible for calling
 * end_progress() on it later (which will free the memory that it uses).
 */
struct progress* start_progress(void) {
    return calloc(1, sizeof(struct progress));
}

/* do_progress(progress, percent, total_bytes_retrieved
 * Updates the supplied progress structure with the new % done given, and
 * recalculates the rolling download rate given the supplied
 * total_bytes_retrieved (and the current time) */
void do_progress(struct progress *p, float pcnt, long long newdl) {
    time_t newtime = time(NULL);
    if (!p->num_history)
        p->starttime = newtime;
    else if (p->history[p->num_history-1].hist_time == newtime)
        return;

    /* Add to the history, rolling off some old history if needed. */
    if (p->num_history >= HISTORY) {
        p->num_history = HISTORY-1;
        memmove(p->history, &(p->history[1]), (HISTORY-1)*sizeof(p->history[0]));
    }
    p->history[p->num_history].hist_time = newtime;
    p->history[p->num_history].dl        = newdl;
    p->history[p->num_history].pcnt      = pcnt;
    p->num_history++;
    
    /* Update progress bar displayed */
    progbar(pcnt * (20.0 / 100.0), pcnt);

    /* If we have more than one data point, we can calculate and show rates */
    if (p->num_history > 1) {
        int passed = p->history[p->num_history-1].hist_time - p->history[0].hist_time;
        float rate = (p->history[p->num_history-1].dl - p->history[0].dl) / (float)passed;
        float pcnt_change = (p->history[p->num_history-1].pcnt - p->history[0].pcnt);
        int sleft = (100.0f - pcnt) * passed / pcnt_change;
        printf(" %.1f kBps ", rate / 1000.0);
        if (sleft < 60 * 1000)
            printf("%d:%02d ETA  ", sleft / 60, sleft % 60);
        else
            fputs("           ", stdout);
    }
    fflush(stdout);
}

/* end_progress(progress, done)
 * Do one final update of the progress display (set to 100% if done is set to
 * true), and then release the progress data structures.
 */
void end_progress(struct progress *p, int done) {
    if (done == 2)
        progbar(20, 100.0);
    else {
        float lastpcnt = p->history[p->num_history-1].pcnt;
        progbar(lastpcnt * (20.0 / 100.0), lastpcnt);
    }

    {   /* For the final display, show the rate for the whole download. */
        float rate = (float)(p->history[p->num_history-1].dl) /
            (p->history[p->num_history-1].hist_time - p->starttime + 0.5);
        printf(" %.1f kBps ", rate / 1000.0);
    }
    puts(done == 2 ? "DONE    \n" : !done ? "aborted    \n" : "        \n");
    fflush(stdout);
    free(p);
}

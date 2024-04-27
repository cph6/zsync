
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

/* HTTP client code for zsync.
 * Including pipeline HTTP Range fetching code.  */

#include "zsglobal.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <unistd.h>

#include <sys/types.h>
#include <sys/stat.h>
#include <sys/socket.h>
#include <netdb.h>
#include <time.h>

#ifndef HAVE_GETADDRINFO
#include "getaddrinfo.h"
#endif

#ifdef WITH_DMALLOC
# include <dmalloc.h>
#endif

#include "http.h"
#include "url.h"
#include "progress.h"
#include "format_string.h"

/* socket = connect_to(host, service/port)
 * Establishes a TCP connection to the named host and port (which can be
 * supplied as a service name from /etc/services. Returns the socket handle, or
 * -1 on error. */
int connect_to(const char *node, const char *service) {
    struct addrinfo hint;
    struct addrinfo *ai;
    int rc;

    memset(&hint, 0, sizeof hint);
    hint.ai_family = AF_UNSPEC;
    hint.ai_socktype = SOCK_STREAM;

    if ((rc = getaddrinfo(node, service, &hint, &ai)) != 0) {
        perror(node);
        return -1;
    }
    else {
        struct addrinfo *p;
        int sd = -1;

        for (p = ai; sd == -1 && p != NULL; p = p->ai_next) {
            if ((sd =
                 socket(p->ai_family, p->ai_socktype, p->ai_protocol)) == -1) {
                perror("socket");
            }
            else if (connect(sd, p->ai_addr, p->ai_addrlen) < 0) {
                perror(node);
                close(sd);
                sd = -1;
            }
        }
        freeaddrinfo(ai);
        return sd;
    }
}

/* fh = http_get_stream(filedesc, &status_code)
 * Converts a socket into a stream, and reads the first line from it as an HTTP
 * status line (response to a request that the caller should have already sent)
 * and returns the stream, and the status code to the location specified by the
 * second parameter. 
 */
FILE *http_get_stream(int fd, int *code) {
    FILE *f = fdopen(fd, "r");
    char buf[256];
    char *p;

    if (fgets(buf, sizeof(buf), f) == NULL || memcmp(buf, "HTTP/1", 6) != 0
        || (p = strchr(buf, ' ')) == NULL) {
        *code = 0;
        fclose(f);
        return NULL;
    }

    *code = atoi(++p);

    return f;
}

/* url = get_location_url(stream, current_url)
 * Reads the HTTP response from the given stream and extracts the Location
 * header, making this URL absolute using the current URL. Returned as a
 * malloced string.
 * (it ought to be absolute anyway, by the RFC, but many servers send 
 * relative URIs). */
char *get_location_url(FILE * f, const char *cur_url) {
    char buf[1024];

    while (fgets(buf, sizeof(buf), f)) {
        char *p;

        /* exit if end of headers */
        if (buf[0] == '\r' || buf[0] == '\n')
            return NULL;

        /* Look for Location header */
        p = strchr(buf, ':');
        if (!p)
            return NULL;
        *p++ = 0;
        if (strcasecmp(buf, "Location"))
            continue;

        /* Skip leading whitespace */
        while (*p == ' ')
            p++;

        {   /* Remove trailing whitespace */
            char *q = p;
            while (*q != '\r' && *q != '\n' && *q != ' ' && *q)
                q++;
            *q = 0;
        }
        if (!*p)
            return NULL;

        /* Return URL after making absolute */
        return make_url_absolute(cur_url, p);
    }
    return NULL;                // TODO
}

/* Settings for HTTP connections - proxy host and port, auth details */
static char *proxy;
static char *pport;
static char **auth_details; /* This is a realloced array with 3*num_auth_details entries */
static int num_auth_details; /* The groups of 3 strings are host, user, pass */

/* Remember referrer */
char *referer;

/* set_proxy_from_string(str)
 * Sets the proxy settings for HTTP connections to use; these can be either as
 * a host[:port] or as http://host[:port].
 * Returns non-zero if the settings were obtained successfully. */
int set_proxy_from_string(const char *s) {
    if (!memcmp(s, http_scheme, strlen(http_scheme))) {
        /* http:// style proxy string */
        proxy = malloc(256);
        if (!proxy)
            return 0;
        if (!get_http_host_port(s, proxy, 256, &pport))
            return 0;
        if (!pport) {
            pport = strdup("webcache");
        }
        return 1;
    }
    else {
        /* host:port style proxy string; have to parse this ourselves */
        char *p;
        proxy = strdup(s);
        p = strchr(proxy, ':');
        if (!p) {
            pport = strdup("webcache");
            return 1;
        }
        *p++ = 0;
        pport = strdup(p);
        return 1;
    }
}

/* add_auth(host, user, pass)
 * Specify user & password combination to use connecting to the given host.
 */
void add_auth(char *host, char *user, char *pass) {
    auth_details =
        realloc(auth_details, (num_auth_details + 1) * sizeof *auth_details);
    auth_details[num_auth_details * 3] = host;
    auth_details[num_auth_details * 3 + 1] = user;
    auth_details[num_auth_details * 3 + 2] = pass;
    num_auth_details++;
}

/* str = get_auth_hdr(host)
 * For the given host, returns the extra HTTP header(s) that should be included
 * to provide authentication information. Returned as a malloced string.
 */
const char auth_header_tmpl[] = { "Authorization: Basic %s\r\n" };

static char *get_auth_hdr(const char *hn) {
    /* Find any relevant entry in the auth table */
    int i;
    for (i = 0; i < num_auth_details * 3; i += 3) {
        if (!strcasecmp(auth_details[i], hn)) {
            char *b;
            char *header;

            /* We have found an entry in the auth details table for this
             * hostname; get the user & pass to use */
            char *u = auth_details[i + 1];
            char *p = auth_details[i + 2];

            /* Store unencoded user:pass */
            size_t l = strlen(u) + strlen(p) + 2;
            char *w = malloc(l);
            snprintf(w, l, "%s:%s", u, p);

            /* Now base64-encode that, and compose the header */
            b = base64(w);
            l = strlen(b) + strlen(auth_header_tmpl) + 1;
            header = malloc(l);
            snprintf(header, l, auth_header_tmpl, b);

            /* And clean up */
            free(w);
            free(b);
            return header;
        }
    }
    return NULL;
}

/* http_date_string(time, buf, buflen)
 * Stores a valid ASCII representation of the supplied datetime in the supplied
 * buffer (length given as buflen). Returns non-NULL if successful.
 */
static char *http_date_string(time_t t, char *const buf, const int blen) {
    struct tm d;

    if (gmtime_r(&t, &d) != NULL) {
        if (strftime(buf, blen, "%a, %d %h %Y %T GMT", &d) > 0) {
            return buf;
        }
    }
    return NULL;
}

FILE *http_get(const char *orig_url, char **track_referer, const char *tfname) {
    int allow_redirects = 5;
    char *url;
    FILE *f = NULL;
    FILE *g = NULL;
    char *fname = NULL;
    char ifrange[200] = { "" };
    char *authhdr = NULL;
    int code;

    /* If we have a (possibly older or incomplete) copy of this file already,
     * add a suitable headers to only retrieve new/additional content */
    if (tfname) {
        struct stat st;

        /* Construct the name of the incomplete transfer file that would have
         * been used by a previous transfer */
        fname = malloc(strlen(tfname) + 6);
        strcpy(fname, tfname);
        strcat(fname, ".part");

        /* If we have an incomplete previous transfer, then our complete copy
         * must be older but the incomplete copy may be current still and we
         * could continue from that. */
        if (stat(fname, &st) == 0) {
            char buf[50];
            if (http_date_string(st.st_mtime, buf, sizeof(buf)) != NULL)
                snprintf(ifrange, sizeof(ifrange),
                         "If-Unmodified-Since: %s\r\nRange: bytes=" OFF_T_PF
                         "-\r\n", buf, st.st_size);
        }
        else if (errno == ENOENT && stat(tfname, &st) == 0) {
            /* Else, if we have a complete possibly-old version, so only transfer
             * if the remote has newer. */
            char buf[50];
            if (http_date_string(st.st_mtime, buf, sizeof(buf)) != NULL)
                snprintf(ifrange, sizeof(ifrange), "If-Modified-Since: %s\r\n",
                         buf);
        }
    }

    /* Take a malloced copy of the URL, so we treat it the same as strduped
     * URLs for any redirects followed. */
    url = strdup(orig_url);
    if (!url) {
        free(fname);
        return NULL;
    }

    /* Loop for redirect handling */
    for (; allow_redirects-- && url && !f;) {
        char hostn[256];
        const char *connecthost;
        char *connectport;
        char *p;
        char *port;

        /* Extract host and port to connect to */
        if ((p = get_http_host_port(url, hostn, sizeof(hostn), &port)) == NULL)
            break;
        if (!proxy) {
            connecthost = hostn;
            connectport = strdup(port);
        }
        else {
            connecthost = proxy;
            connectport = strdup(pport);
        }

        {   /* Connect */
            int sfd = connect_to(connecthost, connectport);
            free(connectport);
            if (sfd == -1)
                break;

            {   /* Compose request */
                char buf[1024];
                snprintf(buf, sizeof(buf),
                         "GET %s HTTP/1.0\r\nHost: %s%s%s\r\nUser-Agent: zsync/%s\r\n%s%s\r\n",
                         proxy ? url : p, hostn, !strcmp(port,
                                                         "http") ? "" : ":",
                         !strcmp(port, "http") ? "" : port, VERSION,
                         ifrange[0] ? ifrange : "", authhdr ? authhdr : "");

                /* Send request to remote */
                if (send(sfd, buf, strlen(buf), 0) == -1) {
                    perror("sendmsg");
                    close(sfd);
                    break;
                }
            }

            /* Wrap the socket in a stream for convenient line reading of the
             * response. */
            f = http_get_stream(sfd, &code);
            if (!f)
                break;

            /* Redirect - go around again with new URL. */
            if (code == 301 || code == 302 || code == 307) {
                char *oldurl = url;
                url = get_location_url(f, oldurl);
                free(oldurl);
                fclose(f);
                f = NULL;
            }
            else if (code == 401) {   /* Authorization required */
                authhdr = get_auth_hdr(hostn);
                if (authhdr) { /* Go around again with auth header */
                    fclose(f);
                    f = NULL;
                }
                else { /* No auth details available for this host - error out */
                    fclose(f);
                    f = NULL;
                    break;
                }
            }
            else if (code == 412) {     // Precondition (i.e. if-unmodified-since) failed
                ifrange[0] = 0;
                fclose(f);
                f = NULL;       // and go round again without the conditional Range:
            }
            else if (code == 200) {     // Downloading whole file
                /* Write new file (plus allow reading once we finish) */
                g = fname ? fopen(fname, "w+") : tmpfile();
            }
            else if (code == 206 && fname) {    // Had partial content and server confirms not modified
                /* Append to existing on-disk content (plus allow reading once we finish) */
                g = fopen(fname, "a+");
            }
            else if (code == 304) {     // Unchanged (if-modified-since was false)
                /* No fetching, just reuse on-disk file */
                g = fopen(tfname, "r");
            }
            else {                      /* Don't know - error */
                fclose(f);
                f = NULL;
                break;
            }
        }
    }

    /* Store the referrer - we'll supply this when retrieving any content
     * referrer to by this file retrieved. */
    if (track_referer)
        *track_referer = url;
    else
        free(url);

    /* If we got a 304 Not Modified, return the existing content as-is */
    if (code == 304) {
        fclose(f);
        free(fname);
        return g;
    }

    /* Return errors from the above loop */
    if (!f) {
        fprintf(stderr, "failed on url %s\n", url ? url : "(missing redirect)");
        return NULL;
    }

    /* If our open of the output file failed, flag that error */
    if (!g) {
        fclose(f);
        perror("fopen");
        return NULL;
    }

    {   /* Read data returned by the request above, writing to the output file */
        size_t len = 0;
        {   /* Skip headers. TODO support content-encodings, Content-Location etc */
            char buf[512];
            do {
                if (fgets(buf, sizeof(buf), f) == NULL) {
                    perror("read");
                    exit(1);
                }

                sscanf(buf, "Content-Length: " SIZE_T_PF, &len);

            } while (buf[0] != '\r' && !feof(f));
        }

        {   /* Now the actual content. Show progress as we go. */
            size_t got = 0;
            struct progress *p;
            size_t r;

            if (!no_progress) {
                p = start_progress();
                do_progress(p, 0, got);
            }

            while (!feof(f)) {
                /* Read from the network */
                char buf[1024];
                r = fread(buf, 1, sizeof(buf), f);
                if (r == 0 && ferror(f)) {
                    perror("read");
                    break;
                }

                /* And write anything received to the temp file */
                if (r > 0) {
                    if (r > fwrite(buf, 1, r, g)) {
                        fprintf(stderr, "short write on %s\n", fname);
                        break;
                    }

                    /* And maintain progress indication */
                    got += r;
                    if (!no_progress)
                        do_progress(p, len ? (100.0 * got / len) : 0, got);
                }
            }
            if (!no_progress)
                end_progress(p, feof(f) ? 2 : 0);
        }
        fclose(f);
    }

    /* The caller wants the content we just downloaded; return the handle to
     * the start of the file that we have just written. */
    rewind(g);

    /* If we are keeping the download too, move it to the desired name. */
    if (fname) {
        rename(fname, tfname);
        free(fname);
    }

    return g;
}

/****************************************************************************
 *
 * HTTP Range: / 206 response interface 
 * 
 * The state engine here is:
 * If sd == -1, not connected;
 * else, if block_left is 0
 *     if boundary is unset, we're reading HTTP headers
 *     if boundary is set, we're reading a MIME boundary
 * else we're reading a block of actual data; block_left bytes still to read.
 */

struct range_fetch {
    /* URL to retrieve from, host:port, auth header */
    char *url;
    char hosth[256];
    char *authh;

    /* Host and port to connect to (could be the same as the URL, or proxy) */
    char *chost;
    char *cport;

    int sd;         /* Currently open socket to the server, or -1 */
    char *boundary; /* If we're in the middle of reading a mime/multipart
                     * response, this is the boundary string. */

    /* State for block currently being read */
    size_t block_left;  /* non-zero if we're in the middle of reading a block */
    off_t offset;       /* and this is the offset of the start of the block we are reading */

    /* Buffering of data from the remote server */
    char buf[4096];
    int buf_start, buf_end; /* Bytes buf_start .. buf_end-1 in buf[] are valid */

    /* Keep count of total bytes retrieved */
    off_t bytes_down;

    int server_close; /* 0: can send more, 1: cannot send more (but one set of headers still to read), 2: cannot send more and all existing headers read */

    /* Byte ranges to fetch */
    off_t *ranges_todo; /* Contains 2*nranges ranges, consisting of start and stop offset */
    int nranges;
    int rangessent;     /* We've requested the first rangessent ranges from the remote */
    int rangesdone;     /* and received this many */
};

/* range_fetch methods */

/* range_fetch_set_url(rf, url)
 * Set up a range_fetch to fetch from a given URL. Private method. 
 * C is a nightmare for memory allocation here. At least the errors should be
 * caught, but minor memory leaks may occur on some error paths. */
static int range_fetch_set_url(struct range_fetch* rf, const char* orig_url) {
    /* Get the host, port and path from the URL. */
    char hostn[sizeof(rf->hosth)];
    char* cport;
    char* p = get_http_host_port(orig_url, hostn, sizeof(hostn), &cport);
    if (!p) {
        return 0;
    }

    free(rf->url);
    if (rf->authh) free(rf->authh);

    /* Get host:port for Host: header */
    if (strcmp(cport, "http") != 0)
        snprintf(rf->hosth, sizeof(rf->hosth), "%s:%s", hostn, cport);
    else
        snprintf(rf->hosth, sizeof(rf->hosth), "%s", hostn);

    if (proxy) {
        /* URL must be absolute; don't need cport anymore, just need full URL
         * to give to proxy. */
        free(cport);
        rf->url = strdup(orig_url);
    }
    else {
        free(rf->cport);
        free(rf->chost);
        // Set url to relative part and chost, cport to the target
        if ((rf->chost = strdup(hostn)) == NULL) {
            free(cport);
            return 0;
        }
        rf->cport = cport;
        rf->url = strdup(p);
    }

    /* Get any auth header that we should use */
    rf->authh = get_auth_hdr(hostn);

    return !!rf->url;
}

/* get_more_data - this is the method which owns all reads from the remote.
 * Nothing else reads from the remote. This buffers data, so that the
 * higher-level methods below can easily read whole lines from the remote. 
 * The higher-level methods call this function when they need more data: 
 * it refills the buffer with data from the network. Returns the bytes read. */
static int get_more_data(struct range_fetch *rf) {
    /* First, garbage collect - move the 'live' data in the buffer to the start
     * of the buffer. */
    if (rf->buf_start) {
        memmove(rf->buf, &(rf->buf[rf->buf_start]),
                rf->buf_end - rf->buf_start);
        rf->buf_end -= rf->buf_start;
        rf->buf_start = 0;
    }

    {   /* Read as much as the OS wants to give us, up to a limit of filling
         * the rest of the buffer; ignore EINTR. */
        int n;
        do {
            n = read(rf->sd, &(rf->buf[rf->buf_end]),
                     sizeof(rf->buf) - rf->buf_end);
        } while (n == -1 && errno == EINTR);
        if (n < 0) {
            perror("read");
        }
        else {

            /* Add new bytes to buffer, and update total bytes count */
            rf->buf_end += n;
            rf->bytes_down += n;
        }
        return n;
    }
}

/* rfgets - get next line from the remote (terminated by LF or end-of-file)
 * (using the buffer, fetching more data if there's no full line in the buffer
 * yet) */
static char *rfgets(char *buf, size_t len, struct range_fetch *rf) {
    char *p;
    while (1) {
        /* Look for a line end in the in buffer */
        p = memchr(rf->buf + rf->buf_start, '\n', rf->buf_end - rf->buf_start);

        /* If we don't have the end of the line yet, fetch more data into the
         * buffer (and go around again) */
        if (!p) {
            int n = get_more_data(rf);
            if (n <= 0) {
                /* EOF - just return all that we have left */
                p = &(rf->buf[rf->buf_end]);
            }
        }
        else    /* We have a \n; set p to point just past it */
            p++;

        if (p) {
            register char *bufstart = &(rf->buf[rf->buf_start]);

            /* Work out how much data to return - the line, or at most 'len' bytes */
            len--;              /* leave space for trailing \0 */
            if (len > (size_t) (p - bufstart))
                len = p - bufstart;

            /* Copy from input buffer to return buffer, nul terminate, and advance
             * current position in the input buffer */
            memcpy(buf, bufstart, len);
            buf[len] = 0;
            rf->buf_start += len;
            return buf;
        }
    }
}

/* range_fetch_start(origin_url)
 * Returns a new range fetch object, for the given URL.
 */
struct range_fetch *range_fetch_start(const char *orig_url) {
    struct range_fetch *rf = malloc(sizeof(struct range_fetch));
    if (!rf)
        return NULL;

    /* If going through a proxy, we can immediately set up the host and port to
     * connect to */
    if (proxy) {
        rf->cport = strdup(pport);
        rf->chost = strdup(proxy);
    }
    else {
        rf->cport = NULL;
        rf->chost = NULL;
    }
    /* Blank initialisation for other fields before set_url call */
    rf->url = NULL;
    rf->authh = NULL;

    if (!range_fetch_set_url(rf, orig_url)) {
        free(rf->cport);
        free(rf->chost);
        free(rf);
        return NULL;
    }

    /* Initialise other state fields */
    rf->block_left = 0;
    rf->bytes_down = 0;
    rf->boundary = NULL;
    rf->sd = -1;                        /* Socket not open */
    rf->ranges_todo = NULL;             /* And no ranges given yet */
    rf->nranges = rf->rangesdone = 0;

    return rf;
}

/* range_fetch_addranges(self, off_t[], nranges)
 * Adds ranges to fetch, supplied as an array of 2*nranges offsets (start and
 * stop for each range) */
void range_fetch_addranges(struct range_fetch *rf, off_t * ranges, int nranges) {
    int existing_ranges = rf->nranges - rf->rangesdone;

    /* Allocate new memory, enough for valid existing entries and new entries */
    off_t *nr = malloc(2 * sizeof(*ranges) * (nranges + existing_ranges));
    if (!nr)
        return;

    /* Copy only still-valid entries from the existing queue over */
    memcpy(nr, &(rf->ranges_todo[2 * rf->rangesdone]),
           2 * sizeof(*ranges) * existing_ranges);

    /* And replace existing queue with new one */
    free(rf->ranges_todo);
    rf->ranges_todo = nr;
    rf->rangessent -= rf->rangesdone;
    rf->rangesdone = 0;
    rf->nranges = existing_ranges;

    /* And append the new stuff */
    memcpy(&nr[2 * existing_ranges], ranges, 2 * sizeof(*ranges) * nranges);
    rf->nranges += nranges;
}

/* range_fetch_connect
 * Connect this rf to its remote server */
static void range_fetch_connect(struct range_fetch *rf) {
    rf->sd = connect_to(rf->chost, rf->cport);
    rf->server_close = 0;
    rf->rangessent = rf->rangesdone;
    rf->buf_start = rf->buf_end = 0;    /* Buffer initially empty */
}

/* range_fetch_getmore
 * On a connected range fetch, send another request to the remote */
static void range_fetch_getmore(struct range_fetch *rf) {
    char request[2048];
    int l;
    int max_range_per_request = 20;

    /* Only if there's stuff queued to get */
    if (rf->rangessent == rf->nranges)
        return;

    /* Build the base request, everything up to the Range: bytes= */
    snprintf(request, sizeof(request),
             "GET %s HTTP/1.1\r\n"
             "User-Agent: zsync/" VERSION "\r\n"
             "Host: %s"
             "%s%s\r\n"
             "%s"
             "Range: bytes=",
             rf->url, rf->hosth,
             referer ? "\r\nReferer: " : "", referer ? referer : "",
             rf->authh ? rf->authh : "");

    /* The for loop here is just a sanity check, lastrange is the real loop control */
    for (; rf->rangessent < rf->nranges;) {
        int i = rf->rangessent;
        int lastrange = 0;

        /* Add at least one byterange to the request; but is this the last one? 
         * That's decided based on whether there are any more to add, whether
         * we've reached our self-imposed limit per request, and whether
         * there's buffer space to add more.
         */
        l = strlen(request);
        if (l > 1200 || !(--max_range_per_request) || i == rf->nranges - 1)
            lastrange = 1;

        /* Append to the request */
        snprintf(request + l, sizeof(request) - l, OFF_T_PF "-" OFF_T_PF "%s",
                 rf->ranges_todo[2 * i], rf->ranges_todo[2 * i + 1],
                 lastrange ? "" : ",");

        /* And record that we have sent this one */
        rf->rangessent++;

        /* Exit loop if that is the last to add */
        if (lastrange)
            break;
    }
    l = strlen(request);

    /* Possibly close the connection (and record the fact, so we definitely
     * don't send more stuff) if this is the last */
    snprintf(request + l, sizeof(request) - l, "\r\n%s\r\n",
             rf->rangessent == rf->nranges ? (rf->server_close =
                                              1, "Connection: close\r\n") : "");

    {   /* Send the request */
        size_t len = strlen(request);
        char *p = request;
        int r = 0;

        while (len > 0
               && ((r = send(rf->sd, p, len, 0)) != -1 || errno == EINTR)) {
            if (r >= 0) {
                p += r;
                len -= r;
            }
        }
        if (r == -1) {
            perror("send");
        }
    }
}

/* buflwr(str) - in-place convert this string to lower case */
static void buflwr(char *s) {
    char c;
    while ((c = *s) != 0) {
        if (c >= 'A' && c <= 'Z')
            *s = c - 'A' + 'a';
        s++;
    }
}

/* range_fetch_read_http_headers - read a set of HTTP headers, updating state
 * appropriately.
 * Returns: EOF returns 0, good returns 206 (reading a range block) or 30x
 *  (redirect), error returns <0 */
int range_fetch_read_http_headers(struct range_fetch *rf) {
    char buf[512];
    int status;
    int seen_location = 0;

    {                           /* read status line */
        char *p;

        if (rfgets(buf, sizeof(buf), rf) == NULL)
            return -1;
        if (buf[0] == 0)
            return 0;           /* EOF, caller decides if that's an error */
        if (memcmp(buf, "HTTP/1", 6) != 0 || (p = strchr(buf, ' ')) == NULL) {
            fprintf(stderr, "got non-HTTP response '%s'\n", buf);
            return -1;
        }
        status = atoi(p + 1);
        if (status != 206 && status != 301 && status != 302) {
            if (status >= 300 && status < 400) {
                fprintf(stderr,
                        "\nzsync received a redirect/further action required status code: %d\nzsync specifically refuses to proceed when a server requests further action. This is because zsync makes a very large number of requests per file retrieved, and so if zsync has to perform additional actions per request, it further increases the load on the target server. The person/entity who created this zsync file should change it to point directly to a URL where the target file can be retrieved without additional actions/redirects needing to be followed.\nSee http://zsync.moria.orc.uk/server-issues\n",
                        status);
            }
            else if (status == 200) {
                fprintf(stderr,
                        "\nzsync received a data response (code %d) but this is not a partial content response\nzsync can only work with servers that support returning partial content from files. The person/entity creating this .zsync has tried to use a server that is not returning partial content. zsync cannot be used with this server.\nSee http://zsync.moria.orc.uk/server-issues\n",
                        status);
            }
            else {
                /* generic error message otherwise */
                fprintf(stderr, "bad status code %d\n", status);
            }
            return -1;
        }
        if (*(p - 1) == '0') {  /* HTTP/1.0 server? */
            rf->server_close = 2;
        }
    }

    /* Read other headers */
    while (1) {
        char *p;

        /* Get next line */
        if (rfgets(buf, sizeof(buf), rf) == NULL)
            return -1;

        /* If it's the end of the headers */
        if (buf[0] == '\r' || buf[0] == '\0') {
            /* We are happy provided we got the block boundary, or an actual block is starting. */
            if (((rf->boundary || rf->block_left)
                 && !(rf->boundary && rf->block_left))
                || (status >= 300 && status < 400 && seen_location))
                return status;
            break;
        }

        /* Parse header */
        p = strstr(buf, ": ");
        if (!p)
            break;
        *p = 0;
        p += 2;
        buflwr(buf);
        {   /* Remove the trailing \r\n from the value */
            int len = strcspn(p, "\r\n");
            p[len] = 0;
        }
        /* buf is the header name (lower-cased), p the value */
        /* Switch based on header */

        /* If remote closes the connection on us, record that */
        if (!strcmp(buf, "connection") && !strcmp(p, "close")) {
            rf->server_close = 2;
        }

        if (status == 206 && !strcmp(buf, "content-range")) {
            /* Okay, we're getting a non-MIME block from the remote. Get the
             * range and set our state appropriately */
            off_t from, to;
            sscanf(p, "bytes " OFF_T_PF "-" OFF_T_PF "/", &from, &to);
            if (from <= to) {
                rf->block_left = to + 1 - from;
                rf->offset = from;
            }

            /* Can only have got one range. */
            rf->rangesdone++;
            rf->rangessent = rf->rangesdone;
        }

        /* If we're about to get a MIME multipart block set */
        if (status == 206 && !strcasecmp(buf, "content-type")
            && !strncasecmp(p, "multipart/byteranges", 20)) {

            /* Get the multipart boundary string */
            char *q = strstr(p, "boundary=");
            if (!q)
                break;
            q += 9;

            /* Gah, we could really use a regexp here. Could be quoted... */
            if (*q == '"') {
                rf->boundary = strdup(q + 1);
                q = strchr(rf->boundary, '"');
                if (q)
                    *q = 0;
            }
            else {  /* or unquoted */
                rf->boundary = strdup(q);
                q = rf->boundary + strlen(rf->boundary) - 1;

                while (*q == '\r' || *q == ' ' || *q == '\n')
                    *q-- = '\0';
            }
        }

        /* If remote is telling us to change URL */
        if ((status == 302 || status == 301)
            && !strcmp(buf, "location")) {
            if (seen_location++) {
                fprintf(stderr, "Error: multiple Location headers on redirect\n");
                break;
            }

            /* Set new target URL 
             * NOTE: we are violating the "the client SHOULD continue to use
             * the Request-URI for future requests" of RFC2616 10.3.3 for 302s.
             * It's not practical given the number of requests we are making to
             * follow the RFC here, and at least we're only remembering it for
             * the duration of this transfer. */
            if (!no_progress)
                fprintf(stderr, "followed redirect to %s\n", p);
            range_fetch_set_url(rf, p);

            /* Flag caller to reconnect; the new URL might be a new target. */
            rf->server_close = 2;
        }
        /* No other headers that we care about. In particular:
         *
         * FIXME: non-conformant to HTTP/1.1 because we ignore
         * Transfer-Encoding: chunked.
         */
    }
    return -1;
}

/* get_range_block(self, &offset, buf[], buflen)
 *
 * This is where it all happens. This is a complex function to present a very
 * simple read(2)-like interface to the caller over the top of all the HTTP
 * going on.
 *
 * It returns blocks of actual data, retrieved from the origin URL, to the
 * caller. Data is returned in the buffer, up to the specified length, and the
 * offset in the file from which the data comes is written to the offset
 * parameter.
 *
 * Like read(2), it returns the total bytes read, 0 for EOF, -1 for error.
 *
 * The blocks that it returns are the ones previously registered by calls to
 * range_fetch_addranges (although it doesn't guarantee that only those block
 * are returned - that's just what it asks the remote for, but if the remote
 * returns more then it'll pass more to the caller - which doesn't matter).
 */
int get_range_block(struct range_fetch *rf, off_t * offset, unsigned char *data,
                    size_t dlen) {
    size_t bytes_to_caller = 0;

    /* If we're not in the middle of reading a block of actual data */
    if (!rf->block_left) {
      check_boundary:
        /* And if not reading a MIME multipart boundary */
        if (!rf->boundary) {

            /* Then we're reading the start of a new set of HTTP headers
             * (possibly after connecting and sending a request first. */
            int newconn = 0;
            int header_result;

            /* If the server closed the connection on us, close our end. */
            if (rf->sd != -1 && rf->server_close == 2) {
                close(rf->sd);
                rf->sd = -1;
            }

            /* If not connected, connect and immediately request a block */
            if (rf->sd == -1) {
                if (rf->rangesdone == rf->nranges)
                    return 0;
                range_fetch_connect(rf);
                if (rf->sd == -1)
                    return -1;
                newconn = 1;
                range_fetch_getmore(rf);
            }

            /* read the response headers */
            header_result = range_fetch_read_http_headers(rf);

            /* Might be the last */
            if (rf->server_close == 1)
                rf->server_close = 2;

            /* EOF on first connect is fatal */
            if (newconn && header_result == 0) {
                fprintf(stderr, "EOF from %s\n", rf->url);
                return -1;
            }

            /* Return EOF or error to caller */
            if (header_result <= 0)
                return header_result ? -1 : 0;

            /* Reconnect for a redirect */
            if (header_result >= 300 && header_result < 400) {
                rf->server_close = 2;
                goto check_boundary;
            }

            /* HTTP Pipelining - send next request before reading current response */
            if (!rf->server_close)
                range_fetch_getmore(rf);
        }

        /* Okay, if we're (now) reading a MIME boundary */
        if (rf->boundary) {
            /* Throw away blank line */
            char buf[512];
            int gotr = 0;
            if (!rfgets(buf, sizeof(buf), rf))
                return 0;

            /* Get, hopefully, boundary marker line */
            if (!rfgets(buf, sizeof(buf), rf))
                return 0;
            if (buf[0] != '-' || buf[1] != '-')
                return 0;

            if (memcmp(&buf[2], rf->boundary, strlen(rf->boundary))) {
                fprintf(stderr, "got bad block boundary: %s != %s",
                        rf->boundary, buf);
                return -1;      /* This is an error now */
            }

            /* Last record marker has boundary followed by - */
            if (buf[2 + strlen(rf->boundary)] == '-') {
                free(rf->boundary);
                rf->boundary = NULL;
                goto check_boundary;
            }

            /* Otherwise, we're reading the MIME headers for this part until we get \r\n alone */
            for (; buf[0] != '\r' && buf[0] != '\n' && buf[0] != '\0';) {
                off_t from, to;

                /* Get next header */
                if (!rfgets(buf, sizeof(buf), rf))
                    return 0;
                buflwr(buf);  /* HTTP headers are case insensitive */

                /* We're looking for the Content-Range: header, to tell us how
                 * many bytes and what part of the target file they represent.
                 */
                if (2 ==
                    sscanf(buf,
                           "content-range: bytes " OFF_T_PF "-" OFF_T_PF "/",
                           &from, &to)) {
                    rf->offset = from;
                    rf->block_left = to - from + 1;
                    gotr = 1;
                }
            }

            /* If we didn't get the byte range that this block represents, it's busted. */
            if (!gotr) {
                fprintf(stderr,
                        "got multipart/byteranges but no Content-Range?");
                return -1;
            }

            /* Else, record that this range is (being) received */
            rf->rangesdone++;
        }
    }

    /* Now the easy bit - we are reading a block of actual data */
    if (!rf->block_left)
        return 0;   /* pass EOF back to caller */
    *offset = rf->offset;   /* caller wants to know what this data is */

    /* Loop until we've retrieved a whole block */
    for (;;) {
        /* Calculate how much more we can return to the caller now. This is the
         * minimum of:
         *   the amount left in this block from the remote
         *   space left in the caller's buffer
         *   the amount we have actually read from the remote
         */
        size_t rl = rf->block_left;
        if (rl > dlen)
            rl = dlen;
        if ((size_t) (rf->buf_end - rf->buf_start) < rl) {
            rl = rf->buf_end - rf->buf_start;

            /* There is more data in this block, and space for more in the
             * caller's buffer, but we don't have any more read from the remote
             * into our buffer yet. So read more now.
             * If we don't get data, drop through and return what we have got.
             * If we do, back to top of loop and try again.
             */
            if (!rl && get_more_data(rf) > 0)
                continue;
        }

        /* If the caller's buffer is full or there's no more data in this block
         * to give, we can now return. */
        if (!rl)
            return bytes_to_caller;

        /* Copy that amount to the caller's their buffer from our buffer */
        memcpy(data, &(rf->buf[rf->buf_start]), rl);
        rf->buf_start += rl;    /* Track pos in our buffer... */
        data += rl;
        dlen -= rl;             /* ...and caller's */
        bytes_to_caller += rl;  /* ...and the return value */

        /* Keep track of how much of the current block is left to read */
        rf->block_left -= rl;
        /* and what position we are up to in the whole source file */
        rf->offset += rl;
        /* And go around again */
    }
}

/* range_fetch_bytes_down
 * Simple getter method, returns the total bytes retrieved */
off_t range_fetch_bytes_down(const struct range_fetch * rf) {
    return rf->bytes_down;
}

/* Destructor */
void range_fetch_end(struct range_fetch *rf) {
    if (rf->sd != -1)
        close(rf->sd);
    free(rf->ranges_todo);
    free(rf->boundary);
    free(rf->url);
    free(rf->cport);
    free(rf->chost);
    free(rf);
}

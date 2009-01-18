
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

/* Very crude URL parsing module */

#include "zsglobal.h"

#include <string.h>
#include <stdlib.h>

#ifdef WITH_DMALLOC
# include <dmalloc.h>
#endif

#include "url.h"

const char http_scheme[] = { "http://" };

/* path_str = get_http_host_port(url_str, &host, host_len, &port_str)
 * For the given url_str, returns:
 * the hostname, in host[] array provided by caller (up to length host_len)
 * the port (if any) as a malloced string, pointer to this stored in *port_str
 * return value is the path part of the URL, a malloced string.
 * Or return value NULL on failure (host and port could have been written to).
 */
char *get_http_host_port(const char *url, char *hostn, int hnlen, char **port) {
    char *p, *q;

    /* Check it's HTTP */
    if (memcmp(url, http_scheme, strlen(http_scheme)))
        return NULL;

    q = url + strlen(http_scheme);

    p = strchr(q, ':');
    if (p) {                    /* if : is after the first /, we have looked too far ahead */
        char *r = strchr(q, '/');
        if (r && r < p)
            p = NULL;
    }
    if (!p) {
        *port = strdup("http");
        p = strchr(q, '/');
    }

    if (!p)
        return NULL;

    if (p - q < hnlen - 1) {
        memcpy(hostn, q, p - q);
        hostn[p - q] = 0;
    }

    if (*p == ':') {
        size_t l;
        q = p;
        p = strchr(p, '/');
        l = p ? (size_t) (p - q - 1) : strlen(q) - 1;
        *port = malloc(l + 1);
        if (!*port)
            return NULL;
        memcpy(*port, q + 1, l);
        (*port)[l] = 0;
        if (!p)
            p = strdup("/");
    }
    return p;
}

/* abs_url_str = make_url_absolute(base_str, url_str)
 * Returns an absolute version of url_str relative to base_str, as a malloced
 * string. Or NULL on error. */
char * __attribute__ ((pure)) make_url_absolute(const char *base, const char *url) {
    if (is_url_absolute(url))
        return strdup(url);

    /* Otherwise, we'll need a base URL to get the scheme and host */
    if (!base)
        return NULL;

    /* Next, is it a full-path URL? */
    if (*url == '/') {
        size_t l;
        char *newurl;

        /* Find the end of the scheme of the base URL, then the end of the
         * hostname[:port]/ */
        const char *p = strchr(base, ':');
        if (!p || p[1] != '/' || p[2] != '/')
            return NULL;
        p = strchr(p + 3, '/');
        if (!p)    /* We have a http://hostname URL (no trailing / or path) */
            l = strlen(base);
        else       /* http://hostname/... , we want just http://hostname */
            l = p - base;

        /* assert */
        if (base[l] != '/' && base[l] != '\0')
            return NULL;

        /* Append full path to the base http://hostname */
        newurl = malloc(l + strlen(url) + 1);
        memcpy(newurl, base, l);
        strcpy(newurl + l, url);
        return newurl;
    }

    /* No leading / or scheme - relative path */
    {
        int l;
        char *newurl;

        /* Find the end of the path part of the base URL */
        const char *q;
        const char *p = strchr(base, '?');
        if (!p)
            p = strchr(base, '#');
        if (!p)
            p = base + strlen(base);

        /* Find the last / in the path part */
        for (q = p; q > base && *q != '/'; q--);
        if (*q != '/')
            return NULL;

        /* Take the base URL up to and including the last /, and append the relative URL */
        l = q - base + 1;
        newurl = malloc(l + strlen(url) + 1);
        memcpy(newurl, base, l);
        strcpy(newurl + l, url);
        return newurl;
    }
}

/* int n = is_url_absolute(url)
 * Returns 0 if the supplied string is not an absolute URL.
 * Returns the number of characters in the URL scheme if it is.
 */
static const char special[] = { ":/?" };

int __attribute__ ((pure)) is_url_absolute(const char *url) {
    /* find end of first no-special-URL-characters part of the string */
    int n = strcspn(url, special);

    /* If the first special character is a :, the start is a URL scheme */
    if (n > 0 && url[n] == ':')
        return n;

    /* otherwise, it's a full path or relative path URL, or just a local file
     * path (caller knows the context) */
    return 0;
}

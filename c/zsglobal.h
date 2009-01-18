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

#ifndef ZSGLOBAL_H
#define ZSGLOBAL_H


#ifdef HAVE_CONFIG_H
#  include "config.h"
#endif

#if defined(__GNUC__) && defined (__OpenBSD__)
#  define ZS_DECL_BOUNDED(x,y,z) __attribute__((__bounded__(x,y,z)))
#else
#  define ZS_DECL_BOUNDED(x,y,z)
#endif /* ZS_DECL_BOUNDED */

#endif


AUTOMAKE_OPTIONS = check-news
ACLOCAL_AMFLAGS = -I autotools

SUBDIRS = librcksum zlib libzsync doc

bin_PROGRAMS = zsyncmake zsync

zsyncmake_SOURCES = make.c makegz.c makegz.h format_string.h
zsyncmake_LDADD = libzsync/libzsync.a librcksum/librcksum.a zlib/libinflate.a zlib/libdeflate.a -lm

zsync_SOURCES = client.c http.c http.h url.c url.h progress.c progress.h base64.c format_string.h zsglobal.h
zsync_LDADD = libzsync/libzsync.a librcksum/librcksum.a zlib/libinflate.a $(LIBOBJS)

EXTRA_zsync_SOURCES = getaddrinfo.h

# From "GNU autoconf, automake and libtool" Vaughan, Elliston, 
# #  Tromey and Taylor, publisher New Riders, p.134
# # They made me do it! :-)
AUX_DIST_GEN =                          \
        $(ac_aux_dir)/install-sh        \
        $(ac_aux_dir)/missing           \
        $(ac_aux_dir)/ac_c_compile_flags.m4 \
		$(ac_aux_dir)/herrno.m4			\
		$(ac_aux_dir)/in6addr.m4		\
		$(ac_aux_dir)/sockaddrin6.m4	\
		$(ac_aux_dir)/sockinttypes.m4

docdir=$(prefix)/share/doc/@PACKAGE@
doc_DATA = COPYING README
EXTRA_DIST = INSTALL README COPYING $(AUX_DIST_GEN) check-zsyncmake

TESTS_ENVIRONMENT = top_builddir=$(top_builddir)
TESTS = check-zsyncmake


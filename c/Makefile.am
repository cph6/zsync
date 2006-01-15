
AUTOMAKE_OPTIONS = check-news

SUBDIRS = librcksum zlib libzsync doc

bin_PROGRAMS = zsyncmake zsync

zsyncmake_SOURCES = make.c
zsyncmake_LDADD = libzsync/libzsync.a librcksum/librcksum.a zlib/libinflate.a -lm

zsync_SOURCES = client.c http.c http.h url.c url.h
zsync_LDADD = libzsync/libzsync.a librcksum/librcksum.a zlib/libinflate.a

# From "GNU autoconf, automake and libtool" Vaughan, Elliston, 
# #  Tromey and Taylor, publisher New Riders, p.134
# # They made me do it! :-)
AUX_DIST_GEN =                          \
         $(ac_aux_dir)/config.guess      \
         $(ac_aux_dir)/config.sub        \
         $(ac_aux_dir)/install-sh        \
         $(ac_aux_dir)/missing           \
         $(ac_aux_dir)/ac_c_compile_flags.m4

docdir=$(prefix)/share/doc/@PACKAGE@
doc_DATA = COPYING README
EXTRA_DIST = bootstrap INSTALL README COPYING $(AUX_DIST_GEN) check-zsyncmake

TESTS_ENVIRONMENT = top_builddir=$(top_builddir)
TESTS = check-zsyncmake


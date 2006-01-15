
SUBDIRS = lib zlib doc

bin_PROGRAMS = zsyncmake zsync

zsyncmake_SOURCES = make.c
zsyncmake_LDADD = lib/libzsync.a zlib/libinflate.a

zsync_SOURCES = client.c http.c http.h fetch.c fetch.h zfetch.c url.c url.h
zsync_LDADD = lib/libzsync.a zlib/libinflate.a

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
EXTRA_DIST = bootstrap INSTALL README COPYING $(AUX_DIST_GEN)


AUTOMAKE_OPTIONS = subdir-objects

noinst_LIBRARIES = librcksum.a

TESTS = rsumtest
noinst_PROGRAMS = rsumtest

rsumtest_SOURCES = rsum.c rsumtest.c hash.c range.c state.c ../progress.c
librcksum_a_SOURCES = internal.h rcksum.h rsum.c hash.c state.c range.c

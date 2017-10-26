

noinst_LIBRARIES = librcksum.a

TESTS = md4test rsumtest
#noinst_PROGRAMS = md4test rsumtest

md4test_SOURCES = md4test.c md4.h md4.c
rsumtest_SOURCES = rsum.c rsumtest.c hash.c range.c state.c md4.c ../progress.c
librcksum_a_SOURCES = internal.h rcksum.h md4.h rsum.c hash.c state.c range.c md4.c

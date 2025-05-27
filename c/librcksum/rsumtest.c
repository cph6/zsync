#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/time.h>

#include "rcksum.h"
#include "md4.h"
#include "internal.h"

static void test_eq(int a, int b) {
    if (a != b) {
        fprintf(stderr, "%x != %x\n", a, b);
        exit(1);
    }
}

void make_0000ff00_data(unsigned char* data, size_t len) {
    int i;
    for (i = 0; i < len; i++) {
        if (i % 4 == 2) {
            data[i] = 0xff;
        } else data[i] = 0x00;
    }
}

void test_00000000(void) {
    unsigned char data[4096];

    memset(data, 0, sizeof(data));

    struct rsum r = rcksum_calc_rsum_block(data, sizeof(data));
    test_eq(r.a, 0x0000);
    test_eq(r.b, 0x0000);
}

void test_abcde(void) {
    unsigned char data[4096];
    int i;

    for (i = 0; i < sizeof(data); i++) {
        data[i] = "abcde"[i % 5];
    }

    struct rsum r = rcksum_calc_rsum_block(data, sizeof(data));
    test_eq(r.a, 0x2ffe);
    test_eq(r.b, 0xf800);
}

void test_fc000000(void) {
    unsigned char data[4096];

    make_0000ff00_data(data, sizeof(data));

    struct rsum r = rcksum_calc_rsum_block(data, sizeof(data));
    test_eq(r.a, 0xfc00);
    test_eq(r.b, 0x0000);
}

void perf_test_fc000000(int n) {
    struct timeval start, end;
    unsigned char data[4096];
    int i;
    volatile int unused = 0;

    make_0000ff00_data(data, sizeof(data));

    gettimeofday(&start, NULL);
    for (i = 0; i < n; i++) {
        struct rsum r = rcksum_calc_rsum_block(data, sizeof(data));
        unused += r.a + r.b;
    }
    gettimeofday(&end, NULL);

    int took_us = (end.tv_sec - start.tv_sec) * 1000000L + end.tv_usec - start.tv_usec;
    printf("%d iterations, took %d.%06ds\n", n, took_us / 1000000, took_us % 1000000);
}

void main(void) {
    test_00000000();
    test_abcde();
    test_fc000000();

#if 0
    perf_test_fc000000(10000000);
#endif
}

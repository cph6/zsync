/*	$OpenBSD: sha1.h,v 1.23 2004/06/22 01:57:30 jfb Exp $	*/

/*
 * SHA-1 in C
 * By Steve Reid <steve@edmweb.com>
 * 100% Public Domain
 */

#ifndef _SHA1_H
#define _SHA1_H

#include "config.h"

#ifdef HAVE_INTTYPES_H
#include <inttypes.h>
#else
#include <sys/types.h>
#endif

#define	SHA1_BLOCK_LENGTH		64
#define	SHA1_DIGEST_LENGTH		20
#define	SHA1_DIGEST_STRING_LENGTH	(SHA1_DIGEST_LENGTH * 2 + 1)

typedef struct {
    uint32_t state[5];
    uint64_t count;
    uint8_t buffer[SHA1_BLOCK_LENGTH];
} SHA1_CTX;

void SHA1Init(SHA1_CTX *);
void SHA1Pad(SHA1_CTX *);
void SHA1Transform(uint32_t [5], const uint8_t [SHA1_BLOCK_LENGTH])
		ZS_DECL_BOUNDED(__minbytes__,1,5)
		ZS_DECL_BOUNDED(__minbytes__,2,SHA1_BLOCK_LENGTH);
void SHA1Update(SHA1_CTX *, const uint8_t *, size_t)
		ZS_DECL_BOUNDED(__string__,2,3);
void SHA1Final(uint8_t [SHA1_DIGEST_LENGTH], SHA1_CTX *)
		ZS_DECL_BOUNDED(__minbytes__,1,SHA1_DIGEST_LENGTH);
char *SHA1End(SHA1_CTX *, char *)
		ZS_DECL_BOUNDED(__minbytes__,2,SHA1_DIGEST_STRING_LENGTH);
char *SHA1File(const char *, char *)
		ZS_DECL_BOUNDED(__minbytes__,2,SHA1_DIGEST_STRING_LENGTH);
char *SHA1FileChunk(const char *, char *, off_t, off_t)
		ZS_DECL_BOUNDED(__minbytes__,2,SHA1_DIGEST_STRING_LENGTH);
char *SHA1Data(const uint8_t *, size_t, char *)
		ZS_DECL_BOUNDED(__string__,1,2)
		ZS_DECL_BOUNDED(__minbytes__,3,SHA1_DIGEST_STRING_LENGTH);

#define HTONDIGEST(x) do {                                              \
        x[0] = htonl(x[0]);                                             \
        x[1] = htonl(x[1]);                                             \
        x[2] = htonl(x[2]);                                             \
        x[3] = htonl(x[3]);                                             \
        x[4] = htonl(x[4]); } while (0)

#define NTOHDIGEST(x) do {                                              \
        x[0] = ntohl(x[0]);                                             \
        x[1] = ntohl(x[1]);                                             \
        x[2] = ntohl(x[2]);                                             \
        x[3] = ntohl(x[3]);                                             \
        x[4] = ntohl(x[4]); } while (0)

#endif /* _SHA1_H */

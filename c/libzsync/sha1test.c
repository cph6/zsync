/*
 *   zsync - client side rsync over http
 *   Copyright (C) 2005 Colin Phipps <cph@moria.org.uk>
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

#include "zsglobal.h"

#include <string.h>
#include <stdlib.h>
#include <sys/types.h>
#include "sha1.h"

// From RFC3174
const char correct_checksum[SHA1_DIGEST_LENGTH] = {0xA9, 0x99, 0x3E, 0x36, 0x47, 0x06, 0x81, 0x6A, 0xBA, 0x3E, 0x25, 0x71, 0x78, 0x50, 0xC2, 0x6C, 0x9C, 0xD0, 0xD8, 0x9D};

int main(void)
{
	SHA1_CTX ctx;

	SHA1Init(&ctx);

	SHA1Update(&ctx,(uint8_t*)"a",1);
	SHA1Update(&ctx,(uint8_t*)"bc",2);
	{
		uint8_t digest[SHA1_DIGEST_LENGTH];
		SHA1Final(digest,&ctx);
		exit(memcmp(digest,correct_checksum,SHA1_DIGEST_LENGTH));
	}

    return 0;
}

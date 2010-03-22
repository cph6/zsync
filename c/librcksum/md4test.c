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
#include "md4.h"

// From RFC1320
const char correct_checksum[MD4_DIGEST_LENGTH] = {0xd7, 0x9e, 0x1c, 0x30, 0x8a, 0xa5, 0xbb, 0xcd, 0xee, 0xa8, 0xed, 0x63, 0xdf, 0x41, 0x2d, 0xa9 };

int main(void)
{
	MD4_CTX ctx;

	MD4Init(&ctx);

	MD4Update(&ctx,(uint8_t*)"a",1);
	MD4Update(&ctx,(uint8_t*)"bcdefghijklmnopqrstuvwxyz",25);
	{
		uint8_t digest[MD4_DIGEST_LENGTH];
		MD4Final(digest,&ctx);
		exit(memcmp(digest,correct_checksum,MD4_DIGEST_LENGTH));
	}

    return 0;
}

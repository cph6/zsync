#include <string.h>
#include <stdlib.h>
#include <sys/types.h>
#include "sha1.h"

// From RFC3174
const char correct_checksum[SHA1_DIGEST_LENGTH] = {0xA9, 0x99, 0x3E, 0x36, 0x47, 0x06, 0x81, 0x6A, 0xBA, 0x3E, 0x25, 0x71, 0x78, 0x50, 0xC2, 0x6C, 0x9C, 0xD0, 0xD8, 0x9D};

void main(int argc,char** argv)
{
	SHA1_CTX ctx;

	SHA1Init(&ctx);

	SHA1Update(&ctx,"a",1);
	SHA1Update(&ctx,"bc",2);
	{
		uint8_t digest[SHA1_DIGEST_LENGTH];
		SHA1Final(digest,&ctx);
		exit(memcmp(digest,correct_checksum,SHA1_DIGEST_LENGTH));
	}
}

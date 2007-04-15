/*
 *
 * Copyright 1997 Massachusetts Institute of Technology
 *
 * Permission to use, copy, modify, and distribute this software and
 * its documentation for any purpose and without fee is hereby
 * granted, provided that both the above copyright notice and this
 * permission notice appear in all copies, that both the above
 * copyright notice and this permission notice appear in all
 * supporting documentation, and that the name of M.I.T. not be used
 * in advertising or publicity pertaining to distribution of the
 * software without specific, written prior permission.  M.I.T. makes
 * no representations about the suitability of this software for any
 * purpose.  It is provided "as is" without express or implied
 * warranty.
 *
 * THIS SOFTWARE IS PROVIDED BY M.I.T. ``AS IS''.  M.I.T. DISCLAIMS
 * ALL EXPRESS OR IMPLIED WARRANTIES WITH REGARD TO THIS SOFTWARE,
 * INCLUDING, BUT NOT LIMITED TO, THE IMPLIED WARRANTIES OF
 * MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE. IN NO EVENT
 * SHALL M.I.T. BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL,
 * SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT
 * LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF
 * USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND
 * ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY,
 * OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT
 * OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF
 * SUCH DAMAGE.
 */

#include <stdlib.h>
#include <string.h>

/*
 * Base64 encoding
 */
char * base64(const char *src)
{
	static const char base64[] =
	    "ABCDEFGHIJKLMNOPQRSTUVWXYZ"
	    "abcdefghijklmnopqrstuvwxyz"
	    "0123456789+/";
	char *str, *dst;
	size_t l;
	int t, r;

	l = strlen(src);
	if ((str = malloc(((l + 2) / 3) * 4 + 1)) == NULL)
		return (NULL);
	dst = str;
	r = 0;

	while (l >= 3) {
		t = (src[0] << 16) | (src[1] << 8) | src[2];
		dst[0] = base64[(t >> 18) & 0x3f];
		dst[1] = base64[(t >> 12) & 0x3f];
		dst[2] = base64[(t >> 6) & 0x3f];
		dst[3] = base64[(t >> 0) & 0x3f];
		src += 3; l -= 3;
		dst += 4; r += 4;
	}

	switch (l) {
	case 2:
		t = (src[0] << 16) | (src[1] << 8);
		dst[0] = base64[(t >> 18) & 0x3f];
		dst[1] = base64[(t >> 12) & 0x3f];
		dst[2] = base64[(t >> 6) & 0x3f];
		dst[3] = '=';
		dst += 4;
		r += 4;
		break;
	case 1:
		t = src[0] << 16;
		dst[0] = base64[(t >> 18) & 0x3f];
		dst[1] = base64[(t >> 12) & 0x3f];
		dst[2] = dst[3] = '=';
		dst += 4;
		r += 4;
		break;
	case 0:
		break;
	}

	*dst = 0;
	return (str);
}



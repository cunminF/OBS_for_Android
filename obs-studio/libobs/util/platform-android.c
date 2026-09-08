/*
 * Copyright (c) 2023 Lain Bailey <lain@obsproject.com>
 *
 * Permission to use, copy, modify, and distribute this software for any
 * purpose with or without fee is hereby granted, provided that the above
 * copyright notice and this permission notice appear in all copies.
 *
 * THE SOFTWARE IS PROVIDED "AS IS" AND THE AUTHOR DISCLAIMS ALL WARRANTIES
 * WITH REGARD TO THIS SOFTWARE INCLUDING ALL IMPLIED WARRANTIES OF
 * MERCHANTABILITY AND FITNESS. IN NO EVENT SHALL THE AUTHOR BE LIABLE FOR
 * ANY SPECIAL, DIRECT, INDIRECT, OR CONSEQUENTIAL DAMAGES OR ANY DAMAGES
 * WHATSOEVER RESULTING FROM LOSS OF USE, DATA OR PROFITS, WHETHER IN AN
 * ACTION OF CONTRACT, NEGLIGENCE OR OTHER TORTIOUS ACTION, ARISING OUT OF
 * OR IN CONNECTION WITH THE USE OR PERFORMANCE OF THIS SOFTWARE.
 */

#include <stdlib.h>

#include "bmem.h"
#include "platform.h"

/* bionic has no libuuid; platform-nix.c hides its uuid_generate() version behind
 * !__ANDROID__ so this is the only implementation linked into the Android build. */
char *os_generate_uuid(void)
{
	unsigned char buf[16];
	char *out = bmalloc(37);

	arc4random_buf(buf, sizeof(buf));

	buf[6] = (unsigned char)((buf[6] & 0x0F) | 0x40); /* version 4 */
	buf[8] = (unsigned char)((buf[8] & 0x3F) | 0x80); /* variant 10xx */

	snprintf(out, 37, "%02x%02x%02x%02x-%02x%02x-%02x%02x-%02x%02x-%02x%02x%02x%02x%02x%02x", buf[0], buf[1],
		 buf[2], buf[3], buf[4], buf[5], buf[6], buf[7], buf[8], buf[9], buf[10], buf[11], buf[12], buf[13],
		 buf[14], buf[15]);

	return out;
}

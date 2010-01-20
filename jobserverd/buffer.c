/* Copyright (c) 2009 River Tarnell <river@loreley.flyingparchment.org.uk>. */
/*
 * Permission is granted to anyone to use this software for any purpose,
 * including commercial applications, and to alter it and redistribute it
 * freely. This software is provided 'as-is', without any express or implied
 * warranty.
 */

#pragma ident "@(#) $Id$"

#include	<stdlib.h>
#include	<stdio.h>
#include	<string.h>
#include	<stdarg.h>
#include	<assert.h>
#include	<errno.h>
#include	<strings.h>

#include	"buffer.h"
#include	"jobserver.h"

buffer_t *
buf_new()
{
buffer_t	*buf;
	if ((buf = calloc(1, sizeof(*buf))) == NULL)
		logm(LOG_ERR, "buf_new: out of memory");
	return buf;
}

buffer_t *
buf_new_size(size)
	size_t	size;
{
buffer_t	*buf;
	if ((buf = buf_new()) == NULL)
		return NULL;

	if ((buf->b_data = calloc(1, size)) == NULL) {
		logm(LOG_ERR, "buf_new_size: out of memory");
		buf_free(buf);
		return NULL;
	}

	buf->b_size = size;
	return buf;
}

buffer_t *
buf_new_data(data, size)
	char const	*data;
	size_t		 size;
{
buffer_t	*buf;
	if ((buf = buf_new_size(size)) == NULL)
		return NULL;

	(void) memcpy(buf->b_data, data, size);
	return buf;
}

buffer_t *
buf_new_nocopy(data, size)
	char	*data;
	size_t	 size;
{
buffer_t	*buf;
	if ((buf = buf_new()) == NULL)
		return NULL;

	buf->b_data = data;
	buf->b_size = size;
	return buf;
}

void
buf_free(buf)
	buffer_t	*buf;
{
	free(buf->b_data);
	free(buf);
	return;
}

int
buf_resize(buf, size)
	buffer_t	*buf;
	size_t		 size;
{
char	*newd;
	if ((newd = realloc(buf->b_data, size)) == NULL && errno == ENOMEM) {
		logm(LOG_ERR, "buf_resize: out of memory (need %lu bytes)",
				(unsigned long) size);
		return -1;
	}

	if (size > buf->b_size)
		bzero(newd + buf->b_size, size - buf->b_size);

	buf->b_data = newd;
	buf->b_size = size;

	return 0;
}

int
buf_insert(buf, pos, data, size)
	buffer_t	*buf;
	size_t		 pos, size;
	char const	*data;
{
char	*newd;
	if ((newd = realloc(buf->b_data, buf->b_size + size)) == NULL) {
		logm(LOG_ERR, "buf_insert: out of memory");
		return -1;
	}

	buf->b_data = newd;
	if (pos < buf->b_size)
		/* Make room for the new data */
		(void) memcpy(buf->b_data + pos + size,
			buf->b_data + pos,
		/*LINTED*/
			buf->b_size - ((buf->b_data + pos) - buf->b_data));

	(void) memcpy(buf->b_data + pos, data, size);
	buf->b_size += size;
	return 0;
}

int
buf_erase(buf, pos, n)
	buffer_t	*buf;
	size_t		 pos, n;
{
	if (n == 0)
		return 0;

	(void) memcpy(buf->b_data + pos, buf->b_data + pos + n, buf->b_size - (pos + n));
	buf->b_size -= n;
	return 0;
}

int
buf_clear(buf)
	buffer_t	*buf;
{
	free(buf->b_data);
	buf->b_size = 0;
	buf->b_data = 0;
	return 0;
}
#ifdef TEST
void logm(int level, char const *fmt, ...)
{
va_list	ap;
	va_start(ap, fmt);
	(void) vfprintf(stderr, fmt, ap);
	va_end(ap);
}

int
main()
{
buffer_t	*buf;
	buf = buf_new();
	assert(buf);

	buf_insert(buf, 0, "foo", 3);
	assert(buf->b_size == 3);
	assert(memcmp(buf->b_data, "foo", 3) == 0);

	buf_append(buf, "bar", 3);
	assert(buf->b_size == 6);
	assert(memcmp(buf->b_data, "foobar", 6) == 0);

	buf_insert(buf, 3, "quux", 4);
	assert(buf->b_size == 10);
	assert(memcmp(buf->b_data, "fooquuxbar", 10) == 0);

	buf_erase(buf, 0, 3);
	assert(buf->b_size == 7);
	assert(memcmp(buf->b_data, "quuxbar", 7) == 0);

	return 0;
}
#endif	/* TEST */

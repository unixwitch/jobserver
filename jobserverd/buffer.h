/* Copyright (c) 2009 River Tarnell <river@loreley.flyingparchment.org.uk>. */
/*
 * Permission is granted to anyone to use this software for any purpose,
 * including commercial applications, and to alter it and redistribute it
 * freely. This software is provided 'as-is', without any express or implied
 * warranty.
 */

#ifndef BUFFER_H
#define BUFFER_H

#include	<sys/types.h>

/*
 * Helper library for managing a buffer of chars.  Mostly useful for
 * networking.  It's *not* a string (not nul-terminated).
 */
typedef struct buffer {
	char	*b_data;
	size_t	 b_size;
} buffer_t;

/*
 * Create a new, empty buffer (size = 0);
 */
buffer_t *buf_new(void);

/*
 * Create a new buffer and copy the given data into it.
 */
buffer_t *buf_new_data(char const *data, size_t size);

/*
 * Create a new buffer without copying the data.  The buffer takes
 * ownership of the data.
 */
buffer_t *buf_new_nocopy(char *data, size_t size);

/*
 * Create a new zero-filled buffer of the given size.
 */
buffer_t *buf_new_size(size_t size);

/*
 * Free a buffer and the data inside it.
 */
void buf_free(buffer_t *);

/*
 * Change the size of a buffer.  If the new size is smaller than the old
 * size, the extra data will be discarded.  If the new size is larger than
 * the old size, the new data will be zero-filled.
 *
 * Returns 0 on success or -1 if reallocation failed.
 */
int buf_resize(buffer_t *, size_t);


/*
 * Insert data into the buffer at a particular point.  If pos == b_size,
 * data is appended.
 *
 * Returns 0 on success if -1 if reallocation failed.
 */
int buf_insert(buffer_t *, size_t pos, char const *data, size_t dsize);

/*
 * Append data to the buffer.
 */
#define buf_append(b, d, s) buf_insert((b), (b)->b_size, (d), (s))

/*
 * Erase n bytes starting at pos from the buffer.
 */
int buf_erase(buffer_t *, size_t pos, size_t n);

/*
 * Empty the buffer.
 */
int buf_clear(buffer_t *);

#endif	/* !BUFFER_H */

/*
 * Copyright 2010 River Tarnell.  All rights reserved.
 * Use is subject to license terms.
 */

/*
 * A simple key/value database.  The kvdb supports multiple tables inside a
 * single database.
 */

#ifndef KVDB_H
#define	KVDB_H

#include	<sys/types.h>
#include	<dirent.h>

/*
 * A database.  This doesn't contain any data, only tables.  The database
 * directory should not exist before hand; kvdb will create it.
 */

typedef int kvdb_t;

#define	KVDB_CREATE	0x1
kvdb_t	kvdb_open(char const *, int);
void	kvdb_close(kvdb_t);

/*
 * Table operations.
 */

typedef int kvtable_t;

#define	KVT_CREATE	0x1
kvtable_t	kvtable_open(kvdb_t, char const *, int);
void		kvtable_close(kvtable_t);

/*
 * Get/put data from a table.  Keys are nul-terminated strings.  Value are
 * always binary data.  Memory for the returned data will be allocated using
 * malloc() and should be freed by the caller.
 */

int	kvtable_get(kvtable_t, char const *, char **, size_t *);
int	kvtable_get_nvlist(kvtable_t, char const *, nvlist_t **);
int	kvtable_insert(kvtable_t, char const *, char const *, size_t);
int	kvtable_insert_nvlist(kvtable_t, char const *, nvlist_t *);
int	kvtable_replace(kvtable_t, char const *, char const *, size_t);
int	kvtable_replace_nvlist(kvtable_t, char const *, nvlist_t *);
int	kvtable_delete(kvtable_t, char const *);

/*
 * Execute a function for every key in a table.  If the function returns 1,
 * execution will terminate.
 */
typedef int (*kvenumerate_callback) (char const *,
		char const *, size_t, void *);
int	kvenumerate(kvtable_t, kvenumerate_callback, void *);
typedef int (*kvenumerate_nvlist_callback) (char const *,
		nvlist_t *, void *);
int	kvenumerate_nvlist(kvtable_t, kvenumerate_nvlist_callback, void *);

/*
 * Open a cursor that can be used to visit all records.
 */
typedef struct {
	kvtable_t	 table;
	DIR		*dir;
	char		*lastkey;
	char		*lastbuf;
} kvcursor_t;

#define	KVC_EOF	(-2)

kvcursor_t	*kvcursor_open(kvtable_t);
int		 kvcursor_next(kvcursor_t *, char **, char **, size_t *);
void		 kvcursor_close(kvcursor_t *);

#endif	/* KVDB_H */

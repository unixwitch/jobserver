/*
 * Copyright 2010 River Tarnell.  All rights reserved.
 * Use is subject to license terms.
 */

#include	<sys/stat.h>
#include	<sys/mman.h>

#include	<fcntl.h>
#include	<unistd.h>
#include	<errno.h>
#include	<assert.h>
#include	<stdlib.h>
#include	<dirent.h>
#include	<string.h>
#include	<libnvpair.h>

#include	"kvdb.h"

kvdb_t
kvdb_open(directory, flags)
	char const	*directory;
	int		 flags;
{
kvdb_t	f;
	assert(directory);
	assert((flags & ~(KVDB_CREATE)) == 0);
	if ((f = open(directory, O_RDONLY)) == -1 && errno == ENOENT) {
		if (flags & KVDB_CREATE) {
			if (mkdir(directory, 0700) == -1)
				return (-1);
		}

		f = open(directory, O_RDONLY);
	}

	return (f);
}

void
kvdb_close(db)
	kvdb_t	db;
{
	(void) close(db);
}

kvtable_t
kvtable_open(db, name, flags)
	kvdb_t		 db;
	char const	*name;
	int		 flags;
{
kvtable_t	f = -1;
int		cwd = -1;
	assert(db >= 0);
	assert(name);
	assert((flags & ~(KVT_CREATE)) == 0);

	if ((f = openat(db, name, O_RDONLY)) == -1 && errno == ENOENT) {
		if (!(flags & KVT_CREATE))
			return (-1);

		if ((cwd = open(".", O_RDONLY)) == -1)
			goto err;
		if (fchdir(db) == -1)
			goto err;
		if (mkdir(name, 0700) == -1)
			goto err;
		if (fchdir(cwd) == -1)
			goto err;
		(void) close(cwd);
		cwd = -1;

		f = openat(db, name, O_RDONLY);
	}

	return (f);

err:
	if (f != -1)
		(void) close(f);
	if (cwd != -1)
		(void) close(cwd);
	return (-1);
}

void
kvtable_close(table)
	kvtable_t	table;
{
	assert(table >= 0);
	(void) close(table);
}

int
kvtable_get(table, key, rbuf, rsize)
	kvtable_t	table;
	char const	*key;
	char		**rbuf;
	size_t		*rsize;
{
int		fd = -1;
struct stat	sb;
	assert(key);
	assert(rbuf);
	assert(rsize);

	*rbuf = 0;

	if ((fd = openat(table, key, O_RDONLY)) == -1)
		goto err;

	if (fstat(fd, &sb) == -1)
		goto err;

	if ((*rbuf = malloc(sb.st_size)) == NULL)
		goto err;

	errno = EINVAL;
	if (read(fd, *rbuf, sb.st_size) != sb.st_size)
		goto err;

	(void) close(fd);

	*rsize = sb.st_size;
	return (0);

err:
	if (fd != -1)
		(void) close(fd);
	free(*rbuf);
	return (-1);
}

int
kvtable_insert(table, key, buf, size)
	kvtable_t	table;
	char const	*key, *buf;
	size_t		size;
{
int	fd = -1;
	assert(table >= 0);
	assert(key);
	assert(buf);

	if ((fd = openat(table, key, O_WRONLY | O_CREAT | O_EXCL, 0600)) == -1)
		goto err;

	if (write(fd, buf, size) < size)
		goto err;

	if (fsync(fd) == -1)
		goto err;

	if (close(fd) == -1)
		goto err;

	return (0);

err:
	if (fd != -1)
		(void) close(fd);
	return (-1);
}

int
kvtable_replace(table, key, buf, size)
	kvtable_t	table;
	char const	*key, *buf;
	size_t		size;
{
int	fd = -1;
	assert(table >= 0);
	assert(key);
	assert(buf);

	/*LINTED*/
	if ((fd = openat(table, key, O_WRONLY | O_CREAT, 0600)) == -1)
		goto err;

	if (write(fd, buf, size) < size)
		goto err;

	if (fsync(fd) == -1)
		goto err;

	if (close(fd) == -1)
		goto err;

	return (0);

err:
	if (fd != -1)
		(void) close(fd);
	return (-1);
}

int
kvtable_delete(table, key)
	kvtable_t	table;
	char const	*key;
{
	assert(table >= 0);
	assert(key);
	return (unlinkat(table, key, 0));
}

int
kvenumerate(table, callback, udata)
	kvtable_t		 table;
	kvenumerate_callback	 callback;
	void			*udata;
{
int		 fd = -1, ffd = -1;
DIR		*dir = NULL;
struct dirent	*de;
void		*addr;
struct stat	 sb;
	if ((fd = dup(table)) == -1)
		goto err;
	if ((dir = fdopendir(fd)) == NULL)
		goto err;
	while ((de = readdir(dir)) != NULL) {
	int	stop;

		if (*de->d_name == '.')
			continue;

		if ((ffd = openat(table, de->d_name, O_RDONLY)) == -1)
			goto err;
		if (fstat(ffd, &sb) == -1)
			goto err;
		if ((addr = mmap(NULL, sb.st_size, PROT_READ, MAP_PRIVATE,
		    ffd, 0)) == MAP_FAILED)
			goto err;

		stop = callback(de->d_name, addr, sb.st_size, udata);

		(void) munmap(addr, sb.st_size);
		addr = NULL;

		(void) close(ffd);
		ffd = -1;
		if (stop)
			break;
	}

	(void) closedir(dir);
	(void) close(fd);
	return (0);

err:
	if (dir)
		(void) closedir(dir);
	else if (fd != -1)
		(void) close(fd);
	if (addr)
		(void) munmap(addr, sb.st_size);
	if (ffd != -1)
		(void) close(ffd);

	return (-1);
}

int
kvenumerate_nvlist(table, callback, udata)
	kvtable_t	 table;
	void		*udata;
	kvenumerate_nvlist_callback callback;
{
int		 fd = -1, ffd = -1;
DIR		*dir = NULL;
struct dirent	*de;
void		*addr;
struct stat	 sb;
nvlist_t	*nvl = NULL;
	if ((fd = dup(table)) == -1)
		goto err;
	if ((dir = fdopendir(fd)) == NULL)
		goto err;
	while ((de = readdir(dir)) != NULL) {
	int	stop;

		if (*de->d_name == '.')
			continue;

		if ((ffd = openat(table, de->d_name, O_RDONLY)) == -1)
			goto err;
		if (fstat(ffd, &sb) == -1)
			goto err;
		if ((addr = mmap(NULL, sb.st_size, PROT_READ, MAP_PRIVATE,
		    ffd, 0)) == MAP_FAILED)
			goto err;
		if (nvlist_unpack(addr, sb.st_size, &nvl, 0))
			goto err;

		stop = callback(de->d_name, nvl, udata);

		nvlist_free(nvl);
		nvl = NULL;
		(void) munmap(addr, sb.st_size);
		addr = NULL;

		(void) close(ffd);
		ffd = -1;
		if (stop)
			break;
	}

	(void) closedir(dir);
	(void) close(fd);
	return (0);

err:
	if (dir)
		(void) closedir(dir);
	else if (fd != -1)
		(void) close(fd);
	if (addr)
		(void) munmap(addr, sb.st_size);
	if (ffd != -1)
		(void) close(ffd);

	return (-1);
}
kvcursor_t *
kvcursor_open(table)
	kvtable_t	table;
{
kvcursor_t	*curs = NULL;
int		 dfd;
	if ((curs = calloc(1, sizeof (*curs))) == NULL)
		return (NULL);
	curs->table = table;
	if ((dfd = dup(table)) == -1)
		goto err;
	if ((curs->dir = fdopendir(dfd)) == NULL)
		goto err;
	return (curs);

err:
	if (curs && curs->dir)
		(void) closedir(curs->dir);
	else if (dfd != -1)
		(void) close(dfd);
	free(curs);
	return (NULL);
}

void
kvcursor_close(cursor)
	kvcursor_t	*cursor;
{
	free(cursor->lastbuf);
	free(cursor->lastkey);

	(void) closedir(cursor->dir);
	free(cursor);
}

int
kvcursor_next(cursor, rkey, rdata, rsize)
	kvcursor_t	*cursor;
	char		**rkey, **rdata;
	size_t		*rsize;
{
struct dirent	*de;
struct stat	 sb;
int		 fd = -1;

	free(cursor->lastbuf);
	free(cursor->lastkey);
	cursor->lastbuf = cursor->lastkey = NULL;

	*rkey = *rdata = NULL;
	errno = 0;
	if ((de = readdir(cursor->dir)) == NULL) {
		if (errno == 0)
			return (KVC_EOF);
		goto err;
	}

	if ((fd = openat(cursor->table, de->d_name, O_RDONLY)) == -1)
		goto err;

	if (fstat(fd, &sb) == -1)
		goto err;

	if ((*rkey = strdup(de->d_name)) == NULL)
		goto err;

	*rsize = sb.st_size;
	if ((*rdata = malloc(sb.st_size)) == NULL)
		goto err;

	if (read(fd, *rdata, *rsize) < *rsize)
		goto err;

	cursor->lastkey = *rkey;
	cursor->lastbuf = *rdata;
	(void) close(fd);
	return (0);

err:
	if (fd != -1)
		(void) close(fd);
	free(*rkey);
	free(*rdata);
	return (-1);
}

int
kvtable_get_nvlist(table, key, nvl)
	kvtable_t	table;
	char const	*key;
	nvlist_t	**nvl;
{
char	*buf = NULL;
size_t	 bsz;
	if (kvtable_get(table, key, &buf, &bsz) == -1)
		return -1;
	if (nvlist_unpack(buf, bsz, nvl, 0))
		goto err;
	free(buf);
	return 0;
err:
	free(buf);
	return -1;
}

int
kvtable_insert_nvlist(table, key, nvl)
	kvtable_t	table;
	char const	*key;
	nvlist_t	*nvl;
{
char	*buf = NULL;
size_t	 bsz;
	if (nvlist_pack(nvl, &buf, &bsz, NV_ENCODE_NATIVE, 0))
		goto err;
	if (kvtable_insert(table, key, buf, bsz) == -1)
		goto err;
	free(buf);
	return 0;
err:
	free(buf);
	return -1;
}

int
kvtable_replace_nvlist(table, key, nvl)
	kvtable_t	table;
	char const	*key;
	nvlist_t	*nvl;
{
char	*buf = NULL;
size_t	 bsz;
	if (nvlist_pack(nvl, &buf, &bsz, NV_ENCODE_NATIVE, 0))
		goto err;
	if (kvtable_replace(table, key, buf, bsz) == -1)
		goto err;
	free(buf);
	return 0;
err:
	free(buf);
	return -1;
}

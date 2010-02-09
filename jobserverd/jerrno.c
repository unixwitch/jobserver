/*
 * Copyright 2010 River Tarnell.  All rights reserved.
 * Use is subject to license terms.
 */

#include	<errno.h>
#include	<string.h>

#include	"jerrno.h"

static char const *jerrlist[] = {
	"Job not found",
	"Cannot scheduled an enabled job",
	"Invalid schedule format",
	"rctl not found",
	"Invalid rctl",
	"User is not in project",
	"Invalid FMRI",
	"Duplicate FMRI",
	"Cannot delete running job",
	"Job is not in maintenance state",
	"Job is not scheduled",
	"Job is not running",
};
#define nerrs (sizeof(jerrlist) / sizeof(*jerrlist))

const char *
jstrerror(err)
	int	err;
{
	if (err >= 0)
		return strerror(err);

	if (-err >= nerrs)
		return "Unknown error";

	return jerrlist[-err - 1];
}

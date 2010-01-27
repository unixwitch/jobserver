/*
 * Copyright 2010 River Tarnell.  All rights reserved.
 * Use is subject to license terms.
 */

#ifndef	EXECUTE_H
#define	EXECUTE_H

#include	<sys/types.h>

#include	"state.h"

pid_t fork_execute(job_t *, char const *cmd);
int send_mail(char const *recip, char const *msg);

#endif	/* !EXECUTE_H */

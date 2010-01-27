/*
 * Copyright 2010 River Tarnell.  All rights reserved.
 * Use is subject to license terms.
 */

#include	<sys/ctfs.h>
#include	<libcontract.h>
#include	<stdlib.h>
#include	<fcntl.h>
#include	<errno.h>
#include	<string.h>
#include	<unistd.h>
#include	<stdio.h>

#include	"ct.h"
#include	"fd.h"
#include	"jobserver.h"

static ctid_t get_last_ctid(void);

static ctid_t
get_last_ctid()
{
int		 cfd;
ctid_t		 ret;
ct_stathdl_t	 ctst;

	if ((cfd = open64(CTFS_ROOT "/process/latest", O_RDONLY)) == -1) {
		logm(LOG_ERR, "get_last_ctid: %s/process/latest: %s",
		    CTFS_ROOT, strerror(errno));
		return (-1);
	}

	if (ct_status_read(cfd, CTD_COMMON, &ctst) == -1) {
		logm(LOG_ERR, "get_last_ctid: %s/process/latest: %s",
		    CTFS_ROOT, strerror(errno));
		(void) close(cfd);
		return (-1);
	}

	ret = ct_status_get_id(ctst);
	ct_status_free(ctst);
	(void) close(cfd);

	return (ret);
}


contract_t *
contract_open(ctid)
	ctid_t	ctid;
{
contract_t	*ct;
char		 fname[128];

	if ((ct = calloc(1, sizeof (*ct))) == NULL) {
		logm(LOG_ERR, "contract_open: out of memory");
		goto err;
	}

	ct->ct_ctl = ct->ct_events = ct->ct_status = -1;
	ct->ct_id = ctid;

	(void) snprintf(fname, sizeof (fname), "%s/process/%d/ctl",
	    CTFS_ROOT, ctid);

	if ((ct->ct_ctl = open64(fname, O_WRONLY)) == -1) {
		logm(LOG_ERR, "contract_open: %s: %s", fname, strerror(errno));
		goto err;
	}

	if (fd_set_cloexec(ct->ct_ctl, 1) == -1) {
		logm(LOG_ERR, "contract_open: cannot set cloexec on "
			"contract ctl fd: %s",
			strerror(errno));
		goto err;
	}

	(void) snprintf(fname, sizeof (fname), "%s/process/%d/events",
			CTFS_ROOT, ctid);

	if ((ct->ct_events = open64(fname, O_RDONLY)) == -1) {
		logm(LOG_ERR, "contract_open: %s: %s", fname, strerror(errno));
		goto err;
	}

	if (fd_set_cloexec(ct->ct_events, 1) == -1) {
		logm(LOG_ERR, "contract_open: cannot set cloexec on "
				"contract events fd: %s",
				strerror(errno));
		goto err;
	}

	(void) snprintf(fname, sizeof (fname), "%s/process/%d/status",
			CTFS_ROOT, ctid);
	if ((ct->ct_status = open64(fname, O_RDONLY)) == -1) {
		logm(LOG_ERR, "contract_open: %s: %s", fname, strerror(errno));
		goto err;
	}

	if (fd_set_cloexec(ct->ct_status, 1) == -1) {
		logm(LOG_ERR, "contract_open: cannot set cloexec on "
				"contract status fd: %s",
				strerror(errno));
		goto err;
	}

	return (ct);

err:
	contract_close(ct);
	return (NULL);
}

contract_t *
contract_open_latest()
{
ctid_t	id;
	if ((id = get_last_ctid()) == NULL)
		return (NULL);
	return (contract_open(id));
}

void
contract_close(ct)
	contract_t	*ct;
{
	if (!ct)
		return;

	if (ct->ct_events != -1)
		(void) close(ct->ct_events);
	if (ct->ct_status != -1)
		(void) close(ct->ct_status);
	if (ct->ct_ctl != -1)
		(void) close(ct->ct_ctl);
	free(ct);
}

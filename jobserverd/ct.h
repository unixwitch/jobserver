/*
 * Copyright 2010 River Tarnell.  All rights reserved.
 * Use is subject to license terms.
 */

/*
 * A thin wrapper around process contracts.
 */

#ifndef	CT_H
#define	CT_H

#include	<sys/contract/process.h>

typedef struct {
	ctid_t	ct_id;
	int	ct_ctl;
	int	ct_status;
	int	ct_events;
} contract_t;

contract_t	*contract_open(ctid_t id);
contract_t	*contract_open_latest(void);
void		 contract_close(contract_t *);

#endif	/* !CT_H */

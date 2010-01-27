/* Copyright (c) 2010 River Tarnell <river@loreley.flyingparchment.org.uk>. */
/*
 * Permission is granted to anyone to use this software for any purpose,
 * including commercial applications, and to alter it and redistribute it
 * freely. This software is provided 'as-is', without any express or implied
 * warranty.
 */

/*
 * A thin wrapper around process contracts.
 */

#ifndef CT_H
#define CT_H

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

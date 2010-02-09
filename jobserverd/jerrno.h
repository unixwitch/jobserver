/*
 * Copyright 2010 River Tarnell.  All rights reserved.
 * Use is subject to license terms.
 */

#ifndef JERRNO_H
#define JERRNO_H

const char *jstrerror(int);

/* State database errors */
#define JEJOB_NOT_FOUND			-1
#define JECANNOT_SCHEDULE_ENABLED	-2
#define JEINVALID_SCHEDULE		-3
#define JERCTL_NOT_FOUND		-4
#define JEINVALID_RCTL			-5
#define JEUSER_NOT_IN_PROJECT		-6
#define JEINVALID_FMRI			-7
#define JEDUPLICATE_FMRI		-8
#define JECANNOT_DELETE_RUNNING		-9
#define JENOT_IN_MAINTENANCE		-10
#define JENOT_SCHEDULED			-11
#define JENOT_RUNNING			-12
#define JELAST_ERROR			-13

#endif	/* !JERRNO_H */

/* Copyright (c) 2009 River Tarnell <river@loreley.flyingparchment.org.uk>. */
/*
 * Permission is granted to anyone to use this software for any purpose,
 * including commercial applications, and to alter it and redistribute it
 * freely. This software is provided 'as-is', without any express or implied
 * warranty.
 */

#pragma ident "@(#) $Id$"

#ifndef EXECUTE_H
#define EXECUTE_H

#include	<sys/types.h>

#include	"state.h"

pid_t fork_execute(job_t *, char const *cmd);

#endif	/* !EXECUTE_H */

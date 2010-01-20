/* Copyright (c) 2009 River Tarnell <river@loreley.flyingparchment.org.uk>. */
/*
 * Permission is granted to anyone to use this software for any purpose,
 * including commercial applications, and to alter it and redistribute it
 * freely. This software is provided 'as-is', without any express or implied
 * warranty.
 */

#pragma ident "@(#) $Id$"

#ifndef JOBSERVER_H
#define JOBSERVER_H

#include	<syslog.h>
#include	<stdarg.h>

/*PRINTFLIKE2*/
void logm(int sev, char const *msg, ...);

/*
 * Reallocate 'ptr' from o elems to n elems, and zero the
 * new data.
 */
void *xrecalloc(void *ptr, size_t o, size_t n, size_t size);

/*
 * Allocate memory in 's' and snprintf into it.
 */
int vasprintf(char **s, char const *fmt, va_list ap);
int asprintf(char **s, char const *fmt, ...);

#define min(x,y) ((x) < (y) ? (x) : (y))

#define VERSION "E2.0-4_ALPHA"

#ifndef PREFIX
# error prefix not defined
#endif

#define LOGWRITER PREFIX "/lib/logwriter"

#endif	/* JOBSERVER_H */

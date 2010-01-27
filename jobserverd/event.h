/*
 * Copyright 2010 River Tarnell.  All rights reserved.
 * Use is subject to license terms.
 */

/*
 * Simple interface to trigger one-off or repeating events.
 */

#ifndef	EVENT_H
#define	EVENT_H

#include	<port.h>

typedef int ev_id_t;
typedef void (*ev_callback) (ev_id_t, void *);

/*
 * Initialise the event subsystem.
 */
int ev_init(int port);

/*
 * Handle a timer port event.
 */
void ev_handle(port_event_t *);

/*
 * Add an event to run 'when' seconds in the future, and repeat every
 * 'when' seconds from then on.
 */
ev_id_t ev_add(time_t when, ev_callback, void *);

/*
 * Add an event to run once, 'when' seconds in the future.
 */
ev_id_t ev_add_once(time_t when, ev_callback, void *);

/*
 * Cancel an event.
 */
int ev_cancel(ev_id_t);

#endif	/* !EVENT_H */

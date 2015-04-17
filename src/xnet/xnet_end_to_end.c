/* Copyright (C) 1992-2010 I/O Performance, Inc. and the
 * United States Departments of Energy (DoE) and Defense (DoD)
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program in a file named 'Copying'; if not, write to
 * the Free Software Foundation, Inc., 675 Mass Ave, Cambridge, MA 02139.
 */
/* Principal Author:
 *      Tom Ruwart (tmruwart@ioperformance.com)
 * Contributing Authors:
 *       Steve Hodson, DoE/ORNL
 *       Steve Poole, DoE/ORNL
 *       Bradly Settlemyer, DoE/ORNL
 *       Russell Cattelan, Digital Elves
 *       Alex Elder
 * Funding and resources provided by:
 * Oak Ridge National Labs, Department of Energy and Department of Defense
 *  Extreme Scale Systems Center ( ESSC ) http://www.csm.ornl.gov/essc/
 *  and the wonderful people at I/O Performance, Inc.
 */
/*
 * This file contains the subroutines necessary the end-to-end
 * Send and Receive functions. 
 */
#include "xint.h"

/* Generate a useful debug macro */
#define DEBUG 1
#if DEBUG
#define dprintf(flag, ...) if (xgp->global_options & (flag)) fprintf(stderr, __VA_ARGS__)
#define de2eprintf(...) dprintf(GO_DEBUG_E2E, __VA_ARGS__)
#else
#define dprintf(...)
#define de2eprintf(...)
#endif

// forward declarations
static int32_t do_connect(worker_data_t*, int);
static xdd_e2e_ate_t *worker_address_table_entry(worker_data_t*);

static int32_t
do_connect(worker_data_t *wdp, int isdest)
{
	pthread_mutex_t * const mutex = xint_e2e_worker_connection_mutex(wdp);
	(void)pthread_mutex_lock(mutex);

	xni_connection_t * const conn = xint_e2e_worker_connection(wdp);
	//TODO: find a safer test (maybe add something to XNI?)
	if (*conn) {
		// Bail; some other worker has already connected
		(void)pthread_mutex_unlock(mutex);
		return 0;
	}

	int rc = 0;
	target_data_t * const tdp = wdp->wd_tdp;

	// Get this worker's assigned address entry
	xdd_e2e_ate_t *ate = worker_address_table_entry(wdp);

	// Resolve name to an IP address
	rc = xint_lookup_addr(ate->hostname, 0, &wdp->wd_e2ep->e2e_dest_addr);
	assert(0 == rc);
	struct in_addr addr = { .s_addr = wdp->wd_e2ep->e2e_dest_addr };
	char* ip_string = inet_ntoa(addr);

	if (!isdest) {
		fprintf(xgp->errout, "Dest host: %s Connect IP: %s Port: %d\n",
				ate->hostname, ip_string, ate->base_port);
	}
	
	// Create an XNI endpoint from the e2e spec
	xni_endpoint_t xep = {.host = ip_string, .port = ate->base_port};

	// Initialize the set of I/O buffers
	xni_bufset_t bufset;
	memset(&bufset, 0, sizeof(bufset));
	bufset.bufs = tdp->io_buffers;
	// Find the first buffer to be owned by this worker's connection
	for (const xdd_e2e_ate_t *p = tdp->td_e2ep->e2e_address_table;
		 p != ate;
		 p++) {

		bufset.bufs += p->port_count;
	}
	bufset.bufcount = ate->port_count;  // one buffer per port
	bufset.bufsize = tdp->io_buffer_size;
	bufset.reserved = getpagesize();

	// Check for overflow
	assert(bufset.bufs+bufset.bufcount <= tdp->io_buffers+tdp->io_buffers_count);

	if (isdest) {
		rc = xni_accept_connection(tdp->xni_ctx, &xep, &bufset, conn);
	} else {
		rc = xni_connect(tdp->xni_ctx, &xep, &bufset, conn);
	}
	// Translate the error code
	rc = (XNI_OK == rc) ? 0 : -1;

	(void)pthread_mutex_unlock(mutex);
	return rc;
}

int32_t xint_e2e_src_connect(worker_data_t *wdp)
{
	return do_connect(wdp, FALSE);
}

int32_t xint_e2e_dest_connect(worker_data_t *wdp)
{
	return do_connect(wdp, TRUE);
}

/*
 * xint_e2e_disconnect() - close connections and free resources
 *  This function will close all connections associated with the given
 *  target.
 *
 *  Returns 0 on success, -1 on failure
 */
int32_t
xint_e2e_disconnect(target_data_t *tdp)
{
	xint_e2e_t * const e2ep = tdp->td_e2ep;

	// Close all connections
	for (int i = 0; i < e2ep->xni_td_connections_count; i++) {
		int rc = xni_close_connection(e2ep->xni_td_connections+i);
		//TODO: handle errors
		assert(XNI_OK == rc);
	}

	return 0;
}

/*
 * xint_e2e_xni_send() - send the data from source to destination 
 *  Using XNI makes sending a bit weird.  The worker thread read a piece
 *  of data from disk, but it will send whatever XNI has queued.  Hopefully
 *  that will be the read data, but XNI just performs a linear search for a
 *  filled target buffer, so  there is no guarantee.
 *
 *  Release the filled target buffer to XNI
 *  Request a target buffer from XNI
 *  Stitch the new target buffer into wdp over the old one (using the e2ehp)
 *  Send the data
 *
 *  Returns 0 on success, -1 on failure
 */
int32_t xint_e2e_xni_send(worker_data_t *wdp) {
	target_data_t		*tdp;
	xint_e2e_t			*e2ep;		// Pointer to the E2E data struct
	//int 				bytes_sent;		// Cumulative number of bytes sent 
	//int					sento_calls; // Number of times sendto() has been called
	xdd_ts_tte_t		*ttep;		// Pointer to a time stamp table entry

	/* Local aliases */
	tdp = wdp->wd_tdp;
	e2ep = wdp->wd_e2ep;
	
	de2eprintf("DEBUG_E2E: %lld: xdd_e2e_src_send: Target: %d: Worker: %d: ENTER: e2ep=%p\n",(long long int)pclk_now(), tdp->td_target_number, wdp->wd_worker_number, e2ep);

    /* Some timestamp code */
	if (tdp->td_ts_table.ts_options & (TS_ON | TS_TRIGGERED)) {
		ttep = &tdp->td_ts_table.ts_hdrp->tsh_tte[wdp->wd_ts_entry];
		ttep->tte_net_processor_start = xdd_get_processor();
	}

	// Set XNI parameters and send
	xni_target_buffer_set_sequence_number(wdp->wd_task.task_op_number,
										  wdp->wd_e2ep->xni_wd_buf);
	xni_target_buffer_set_target_offset(wdp->wd_task.task_byte_offset,
										wdp->wd_e2ep->xni_wd_buf);
	xni_target_buffer_set_data_length(wdp->wd_task.task_xfer_size,
									  wdp->wd_e2ep->xni_wd_buf);

	de2eprintf("DEBUG_E2E: %lld: xdd_e2e_src_send: Target: %d: Worker: %d: Preparing to send: e2ep=%p: e2eh_data_length=%lld\n",(long long int)pclk_now(), tdp->td_target_number, wdp->wd_worker_number,e2ep,(long long int)xni_target_buffer_data_length(wdp->wd_e2ep->xni_wd_buf));

	nclk_now(&wdp->wd_counters.tc_current_net_start_time);

	xni_connection_t * const connp = xint_e2e_worker_connection(wdp);

	e2ep->e2e_send_status = xni_send_target_buffer(*connp,
												   &e2ep->xni_wd_buf);
	// Request a fresh buffer from XNI
	xni_request_target_buffer(*connp, &wdp->wd_e2ep->xni_wd_buf);

	// Keep a pointer to the data portion of the buffer
	wdp->wd_task.task_datap = xni_target_buffer_data(wdp->wd_e2ep->xni_wd_buf);

	nclk_now(&wdp->wd_counters.tc_current_net_end_time);
	
	// Time stamp if requested
	if (tdp->td_ts_table.ts_options & (TS_ON | TS_TRIGGERED)) {
		ttep = &tdp->td_ts_table.ts_hdrp->tsh_tte[wdp->wd_ts_entry];
		ttep->tte_net_xfer_size = xni_target_buffer_data_length(e2ep->xni_wd_buf);
		ttep->tte_net_start = wdp->wd_counters.tc_current_net_start_time;
		ttep->tte_net_end = wdp->wd_counters.tc_current_net_end_time;
		ttep->tte_net_processor_end = xdd_get_processor();
		ttep->tte_net_xfer_calls = 1;
	}
	
	// Calculate the Send/Receive time by the time it took the last sendto() to run
	e2ep->e2e_sr_time = (wdp->wd_counters.tc_current_net_end_time - wdp->wd_counters.tc_current_net_start_time);

	de2eprintf("DEBUG_E2E: %lld: xdd_e2e_src_send: Target: %d: Worker: %d: EXIT...\n",(long long int)pclk_now(),tdp->td_target_number, wdp->wd_worker_number);

    return(0);

} /* end of xdd_e2e_src_send() */

/*----------------------------------------------------------------------*/
/* xint_e2e_eof_source_side() - End-Of-File processing for Source 
 * Return values: 0 is good, -1 is bad
 */
int32_t
xint_e2e_eof_source_side(worker_data_t *wdp) {
	target_data_t		*tdp;
	xint_e2e_t			*e2ep;			// Pointer to the E2E struct for this worker

	tdp = wdp->wd_tdp;
	e2ep = wdp->wd_e2ep;

if (xgp->global_options & GO_DEBUG_E2E) fprintf(stderr,"DEBUG_E2E: %lld: xdd_e2e_eof_source_side: Target %d Worker: %d: ENTER: \n", (long long int)pclk_now(), tdp->td_target_number, wdp->wd_worker_number);

		e2ep->e2e_send_status = 0;
		e2ep->e2e_sr_time = 0;
		return 0;
} /* end of xdd_e2e_eof_source_side() */

/*
 * xint_e2e_xni_recv() - recv the data from source at destination 
 *
 *  Release the empty target buffer to XNI
 *  Request a target buffer from XNI
 *  Stitch the new target buffer into wdp over the old one (using the e2ehp)
 *  Receive the data
 *
 * Return values: 0 is good, -1 is bad
 */
int32_t xint_e2e_xni_recv(worker_data_t *wdp) {
	target_data_t		*tdp;			// Pointer to the Target Data
	xint_e2e_t			*e2ep;			// Pointer to the E2E struct for this worker
	int32_t				status;			// Status of the call to xdd_e2e_dst_connection()
	nclk_t 				e2e_wait_1st_msg_start_time; // This is the time stamp of when the first message arrived
	xdd_ts_tte_t		*ttep;		// Pointer to a time stamp table entry
	
	/* Collect the begin time */
	nclk_now(&e2e_wait_1st_msg_start_time);
	wdp->wd_counters.tc_current_net_start_time = e2e_wait_1st_msg_start_time;

	/* Receive a target buffer and assemble it into the wdp */
	tdp = wdp->wd_tdp;
	status = xni_receive_target_buffer(*xint_e2e_worker_connection(wdp),
									   &wdp->wd_e2ep->xni_wd_buf);

	if (XNI_OK == status) {
		/* Assemble pointers into the worker's target buffer */
		wdp->wd_task.task_datap = xni_target_buffer_data(wdp->wd_e2ep->xni_wd_buf);
	}
	else if (XNI_EOF == status) {
		wdp->wd_task.task_datap = NULL;
		wdp->wd_e2ep->received_eof = TRUE;
	} else {
		fprintf(xgp->errout, "Error receiving data via XNI.");
		return -1;
	}

	/* Local aliases */
	e2ep = wdp->wd_e2ep;

	/* Collect the end time */
	nclk_now(&wdp->wd_counters.tc_current_net_end_time);

	/* Store timing data */
	e2ep->e2e_sr_time = (wdp->wd_counters.tc_current_net_end_time - wdp->wd_counters.tc_current_net_start_time);
	//e2ehp->e2eh_recv_time = wdp->wd_counters.tc_current_net_end_time; // This needs to be the net_end_time from this side of the operation

	/* Tabulate timestamp data */
	if ((tdp->td_ts_table.ts_options & (TS_ON|TS_TRIGGERED))) {
		ttep = &tdp->td_ts_table.ts_hdrp->tsh_tte[wdp->wd_ts_entry];
		ttep->tte_net_start = wdp->wd_counters.tc_current_net_start_time;
		ttep->tte_net_end = wdp->wd_counters.tc_current_net_end_time;
		ttep->tte_net_processor_end = xdd_get_processor();
		ttep->tte_net_xfer_size = xni_target_buffer_data_length(e2ep->xni_wd_buf);
		ttep->tte_byte_offset = xni_target_buffer_target_offset(e2ep->xni_wd_buf);
		ttep->tte_disk_xfer_size = xni_target_buffer_data_length(e2ep->xni_wd_buf);
		ttep->tte_op_number = xni_target_buffer_sequence_number(e2ep->xni_wd_buf);
		if (wdp->wd_e2ep->received_eof) {
			ttep->tte_op_type = SO_OP_EOF;
		} else {
			ttep->tte_op_type = SO_OP_WRITE;
		}
	}

	return(0);

} /* end of xint_e2e_xni_recv() */

int
xint_is_e2e(const target_data_t *tdp)
{
	return (TO_ENDTOEND == (TO_ENDTOEND & tdp->td_target_options));
}

xni_connection_t*
xint_e2e_worker_connection(worker_data_t *wdp)
{
	const int idx = wdp->wd_e2ep->address_table_index;
	xni_connection_t * const conn = idx >= 0
		? wdp->wd_tdp->td_e2ep->xni_td_connections+idx
		: NULL;

	return conn;
}

pthread_mutex_t*
xint_e2e_worker_connection_mutex(worker_data_t *wdp)
{
	const int idx = wdp->wd_e2ep->address_table_index;
	pthread_mutex_t * const mutex = idx >= 0
		? wdp->wd_tdp->td_e2ep->xni_td_connection_mutexes+idx
		: NULL;

	return mutex;
}

static xdd_e2e_ate_t*
worker_address_table_entry(worker_data_t *wdp)
{
	const int idx = wdp->wd_e2ep->address_table_index;
	xdd_e2e_ate_t * const atep = idx >= 0
		? wdp->wd_tdp->td_e2ep->e2e_address_table+idx
		: NULL;

	return atep;
}

const char*
xint_e2e_worker_dest_hostname(worker_data_t *wdp)
{
	const xdd_e2e_ate_t * const atep = worker_address_table_entry(wdp);
	const char *hostname = atep->hostname;

	return hostname;
}

/*
 * Local variables:
 *  indent-tabs-mode: t
 *  default-tab-width: 4
 *  c-indent-level: 4
 *  c-basic-offset: 4
 * End:
 *
 * vim: ts=4 sts=4 sw=4 noexpandtab
 */

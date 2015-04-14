/*
 * XDD - a data movement and benchmarking toolkit
 *
 * Copyright (C) 1992-23 I/O Performance, Inc.
 * Copyright (C) 2009-23 UT-Battelle, LLC
 *
 * This is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public
 * License version 2, as published by the Free Software
 * Foundation.  See file COPYING.
 *
 *//*
 * This file contains the subroutines that support the Target threads.
 */
#include "xint.h"

/*----------------------------------------------------------------------------*/
/* xdd_target_thread_cleanup() - Perform termination processing of this 
 * this Target thread.
 * Return Values: 0 is good, -1 indicates an error but what are ya gonna do?
 * 
 */
void
xdd_target_thread_cleanup(target_data_t *tdp) {
	worker_data_t	*wdp;		// Pointer to a Worker Thread Data Struct
	
	wdp = tdp->td_next_wdp;
	while (wdp) {
		wdp->wd_task.task_request = TASK_REQ_STOP;
		tdp->td_occupant.occupant_type |= XDD_OCCUPANT_TYPE_CLEANUP;
		// Release this Worker Thread
		xdd_barrier(&wdp->wd_thread_targetpass_wait_for_task_barrier,&tdp->td_occupant,0);

		// get the next Worker in this chain
		wdp = wdp->wd_next_wdp;
	}
	if (tdp->td_target_options & TO_DELETEFILE) {
#ifdef WIN32
		DeleteFile(tdp->td_target_full_pathname);
#else
		unlink(tdp->td_target_full_pathname);
#endif
	}

	// if this is an e2e transfer
	if (xint_is_e2e(tdp)) {
	  // Disconnect
	  xint_e2e_disconnect(tdp);

	  struct xint_e2e * const e2ep = tdp->td_e2ep;

	  // Free the connections
	  e2ep->xni_td_connections_count = 0;
	  free(e2ep->xni_td_connections);
	  e2ep->xni_td_connections = NULL;

	  // Free the connection mutexes
	  for (int i = 0; i < e2ep->xni_td_connections_count; i++) {
	    int error = pthread_mutex_destroy(e2ep->xni_td_connection_mutexes+i);
	    assert(!error);
	  }
	  free(e2ep->xni_td_connection_mutexes);
	  e2ep->xni_td_connection_mutexes = NULL;
	}

	// Free the I/O buffers
	for (size_t i = 0; i < tdp->io_buffers_count; i++) {
	  free(tdp->io_buffers[i]);
	}
	free(tdp->io_buffers);
	tdp->io_buffers = NULL;
	tdp->io_buffers_count = 0;

	/* On non e2e, close the descriptor */
	if (!(TO_ENDTOEND & tdp->td_target_options)) {
		int rc = close(tdp->td_file_desc);
		// Check the status of the CLOSE operation to see if it worked
		if (rc != 0) {
			fprintf(xgp->errout,"%s: xdd_target_open: ERROR: Could not close target number %d name %s\n",
				xgp->progname,
				tdp->td_target_number,
       		                tdp->td_target_full_pathname);
                	fflush(xgp->errout);
                	perror("reason");
		}
	}
    
} // End of xdd_target_thread_cleanup()


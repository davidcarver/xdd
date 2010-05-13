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
 *       Brad Settlemyer, DoE/ORNL
 * Funding and resources provided by:
 * Oak Ridge National Labs, Department of Energy and Department of Defense
 *  Extreme Scale Systems Center ( ESSC ) http://www.csm.ornl.gov/essc/
 *  and the wonderful people at I/O Performance, Inc.
 */
/*
 * This file contains the subroutines necessary to manage everything to
 * do with restarting an xddcp operation.
 */
#include "xdd.h"

// Prototypes
int xdd_restart_create_restart_file(restart_t *rp);
int xdd_restart_write_restart_file(restart_t *rp);

/*----------------------------------------------------------------------------*/
// This routine is called to create a new restart file when a new copy 
// operation is started for the first time.
// The filename of the restart file can be specified by the "-restart file xxx"
// option where xxx will be the file name of the restart file. 
// If the restart file name is not specified then the default location and 
// name will be used. Currently, the default location is the current working
// directory where xddcp is being executed and the file name will be 
//     xdd.$src.$src_basename.$dest.$dest_basename.$gmt_timestamp-GMT.$ext
// where $src is the host name of the source machine
//       $src_basename is the base name of the source file
//       $dest is the host name of the destination machine
//       $dest_basename is the base name of the destination file
//       $gmt_timestamp is the time at which this restart file was created in
//         the form YYYY-MM-DD-hhmm-GMT or year-month-day-hourminutes in GMT
//         and the "-GMT" is appended to the timestamp to indicate this timezone
//       $ext is the file extension which is ".rst" for this type of file
//
int 
xdd_restart_create_restart_file(restart_t *rp) {

	time_t	t;				// Time structure
	struct 	tm	*tm;		// Pointer to the broken-down time struct that lives in the restart struct
	
 
	// Check to see if the file name was provided or not. If not, the create a file name.
	if (rp->restart_filename == NULL) { // Need to create the file name here
		rp->restart_filename = malloc(MAX_TARGET_NAME_LENGTH);
		if (rp->restart_filename == NULL) {
			fprintf(xgp->errout,"%s: RESTART_MONITOR: ALERT: Cannot allocate %d bytes of memory for the restart file name\n",
				xgp->progname,
				MAX_TARGET_NAME_LENGTH);
			perror("Reason");
			rp->fp = xgp->errout;
			return(0);
		}
		// Get the current time in a appropriate format for a file name
		time(&t);
		tm = gmtime_r(&t, &rp->tm);
		sprintf(rp->restart_filename,"xdd.%s.%s.%s.%s.%4d-%02d-%02d-%02d%02d-GMT.rst",
			(rp->source_host==NULL)?"NA":rp->source_host,
			(rp->source_filename==NULL)?"NA":basename(rp->source_filename),
			(rp->destination_host==NULL)?"NA":rp->destination_host,
			(rp->destination_filename==NULL)?"NA":basename(rp->destination_filename),
			tm->tm_year+1900, // number of years since 1900
			tm->tm_mon+1,  // month since January - range 0 to 11 
			tm->tm_mday,   // day of the month range 1 to 31
			tm->tm_hour,
			tm->tm_min);
	}

	// Now that we have a file name lets try to open it in purely write mode.
	rp->fp = fopen(rp->restart_filename,"w");
	if (rp->fp == NULL) {
		fprintf(xgp->errout,"%s: RESTART_MONITOR: ALERT: Cannot create restart file %s!\n",
			xgp->progname,
			rp->restart_filename);
		perror("Reason");
		fprintf(xgp->errout,"%s: RESTART_MONITOR: ALERT: Defaulting to error out for restart file\n",
			xgp->progname);
		rp->fp = xgp->errout;
		rp->restart_filename = NULL;
		free(rp->restart_filename);
		return(-1);
	}
	
	// Success - everything must have worked and we have a restart file
	fprintf(xgp->output,"%s: RESTART_MONITOR: INFO: Successfully created restart file %s\n",
		xgp->progname,
		rp->restart_filename);
	return(0);
} // End of xdd_restart_create_restart_file()

/*----------------------------------------------------------------------------*/
// xdd_restart_write_restart_file() - Update the restart file with current info
// 
// This routine is called to write new information to an existing restart file 
// during a copy operation - this is also referred to as a "checkpoint"
// operation. 
// Each time the write is performed to this file it is flushed to the storage device. 
// 
// 
int
xdd_restart_write_restart_file(restart_t *rp) {
	int		status;

	// Seek to the beginning of the file 
	status = fseek(rp->fp, 0L, SEEK_SET);
	if (status < 0) {
		fprintf(xgp->errout,"%s: RESTART_MONITOR: WARNING: Seek to beginning of restart file %s failed\n",
			xgp->progname,
			rp->restart_filename);
		perror("Reason");
	}
	
	// Issue a write operation for the stuff
	fprintf(rp->fp,"-restart offset %llu\n", 
		(unsigned long long int)rp->last_committed_location);

	// Flush the file for safe keeping
	fflush(rp->fp);

	return(0);
} // End of xdd_restart_write_restart_file()

/*----------------------------------------------------------------------------*/
// This routine is created when xdd starts a copy operation (aka xddcp).
// This routine will run in the background and waits for various xdd I/O
// qthreads to update their respective counters and enter the barrier to 
// notify this routine that a block has been read/written.
// This monitor runs on both the source and target machines during a copy
// operation as is indicated in the name of the restart file. The information
// contained in the restart file has different meanings depending on which side
// of the copy operation the restart file represents. 
// On the source side, the restart file contains information regarding the lowest 
// and highest byte offsets into the source file that have been successfully 
// "read" and "sent" to the destination machine. This does not mean that the 
// destination machine has actually received the data but it is an indicator of 
// what the source side thinks has happened.
// On the destination side the restart file contains information regarding the
// lowest and highest byte offsets and lengths that have been received and 
// written (committed) to stable media. This is the file that should be used
// for an actual copy restart opertation as it contains the most accurate state
// of the destination file.
//
// This routine will also display information about the copy operation before,
// during, and after xddcp is complete. 
// 
void *
xdd_restart_monitor(void *junk) {
	int				target_number;			// Used as a counter
	ptds_t			*current_ptds;			// The current Target Thread PTDS
	uint64_t 		check_counter;			// The current number of times that we have checked on the progress of a copy
	xdd_occupant_t	barrier_occupant;		// Used by the xdd_barrier() function to track who is inside a barrier
	restart_t		*rp;
	int				status;					// Status of mutex init/lock/unlock calls



#ifdef DEBUG
	uint64_t separation;
#endif
	

	// Initialize stuff
	if (xgp->global_options & GO_REALLYVERBOSE)
		fprintf(xgp->output,"%s: xdd_restart_monitor: Initializing...\n", xgp->progname);

	for (target_number=0; target_number < xgp->number_of_targets; target_number++) {
		current_ptds = xgp->ptdsp[target_number];
		rp = current_ptds->restartp;
		status = pthread_mutex_init(&rp->restart_lock, 0);
		if (status) {
			fprintf(stderr,"%s: xdd_restart_monitor: ERROR initializing restart_lock for target %d, status=%d, errno=%d", 
				xgp->progname, 
				target_number, 
				status, 
				errno);
			perror("Reason");
			return(0);
		}
		if (current_ptds->target_options & TO_E2E_DESTINATION) {
			xdd_restart_create_restart_file(rp);
		} else {
			fprintf(xgp->output,"%s: xdd_restart_monitor: INFO: No restart file being created for target %d [ %s ] because this is not the destination side of an E2E operation.\n", 
				xgp->progname,
				current_ptds->my_target_number,
				current_ptds->target_full_pathname);
		}
	}
	if (xgp->global_options & GO_REALLYVERBOSE)
		fprintf(xgp->output,"%s: xdd_restart_monitor: Initialization complete.\n", xgp->progname);

	// Enter this barrier to release main indicating that restart has initialized
	xdd_init_barrier_occupant(&barrier_occupant, "RESTART_MONITOR", (XDD_OCCUPANT_TYPE_SUPPORT), NULL);
	xdd_barrier(&xgp->main_general_init_barrier,&barrier_occupant,0);

	check_counter = 0;
	// This is the loop that periodically checks all the targets/qthreads 
	for (;;) {
		// Sleep for the specified period of time
		sleep(xgp->restart_frequency);

		check_counter++;
		// Check all targets
		for (target_number=0; target_number < xgp->number_of_targets; target_number++) {
			current_ptds = xgp->ptdsp[target_number];
			// If this target does not require restart monitoring then continue
			if ( !(current_ptds->target_options & TO_RESTART_ENABLE) ) // if restart is NOT enabled for this target then continue
				continue;
			
			rp = current_ptds->restartp;
			if (!rp) // Hmmm... no restart pointer..
				continue;
			pthread_mutex_lock(&rp->restart_lock);

			if (rp->flags & RESTART_FLAG_SUCCESSFUL_COMPLETION) {
				pthread_mutex_unlock(&rp->restart_lock);
				continue;
			} else {
				// Put the "Last Committed Block" information in the restart structure...
				rp->last_committed_location = current_ptds->last_committed_location;
				rp->last_committed_length = current_ptds->last_committed_length;
				rp->last_committed_op = current_ptds->last_committed_op;
	
				// ...and write it to the restart file and sync sync sync
				if (current_ptds->target_options & TO_E2E_DESTINATION) // Restart files are only written on the destination side
					xdd_restart_write_restart_file(rp);

			}
			// UNLOCK the restart struct
			pthread_mutex_unlock(&rp->restart_lock);

		} // End of FOR loop that checks all targets 

		// If it is time to leave then leave - the qthread cleanup will take care of closing the restart files
		if (xgp->abort | xgp->canceled) 
			break;
	} // End of FOREVER loop that checks stuff

	fprintf(xgp->output,"%s: RESTART Monitor is exiting\n",xgp->progname);
	return(0);

} // End of xdd_restart_monitor() thread

/*
 * Local variables:
 *  indent-tabs-mode: t
 *  c-indent-level: 4
 *  c-basic-offset: 4
 * End:
 *
 * vim: ts=4 sts=4 sw=4 noexpandtab
 */

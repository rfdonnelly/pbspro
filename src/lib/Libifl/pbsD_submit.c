/*
 * Copyright (C) 1994-2016 Altair Engineering, Inc.
 * For more information, contact Altair at www.altair.com.
 *  
 * This file is part of the PBS Professional ("PBS Pro") software.
 * 
 * Open Source License Information:
 *  
 * PBS Pro is free software. You can redistribute it and/or modify it under the
 * terms of the GNU Affero General Public License as published by the Free 
 * Software Foundation, either version 3 of the License, or (at your option) any 
 * later version.
 *  
 * PBS Pro is distributed in the hope that it will be useful, but WITHOUT ANY 
 * WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS FOR A
 * PARTICULAR PURPOSE.  See the GNU Affero General Public License for more details.
 *  
 * You should have received a copy of the GNU Affero General Public License along 
 * with this program.  If not, see <http://www.gnu.org/licenses/>.
 *  
 * Commercial License Information: 
 * 
 * The PBS Pro software is licensed under the terms of the GNU Affero General 
 * Public License agreement ("AGPL"), except where a separate commercial license 
 * agreement for PBS Pro version 14 or later has been executed in writing with Altair.
 *  
 * Altair’s dual-license business model allows companies, individuals, and 
 * organizations to create proprietary derivative works of PBS Pro and distribute 
 * them - whether embedded or bundled with other software - under a commercial 
 * license agreement.
 * 
 * Use of Altair’s trademarks, including but not limited to "PBS™", 
 * "PBS Professional®", and "PBS Pro™" and Altair’s logos is subject to Altair's 
 * trademark licensing policies.
 *
 */
/**
 * @file	pbs_submit.c
 * @brief
 *	The Submit Job request.
 */

#include <pbs_config.h>   /* the master config generated by configure */

#include <stdio.h>
#include <strings.h>
#include <fcntl.h>
#include <unistd.h>
#include <assert.h>
#include "libpbs.h"
#include "credential.h"
#include "pbs_ecl.h"
#include "pbs_client_thread.h"

#include "ticket.h"


/* for use with pbs_submit_with_cred */
struct cred_info {
	int cred_type;
	size_t cred_len;
	char *cred_buf;
};

/**
 * @brief
 *	-wrapper function for pbs_submit where submission takes credentials.
 *
 * @param[in] c - communication handle
 * @param[in] attrib - pointer to attr list
 * @param[in] script - script to be submitted
 * @param[in] destination - host where submission happens
 * @param[in] extend - extend string for encoding req
 * @param[in] credtype - credential type
 * @param[in] credlen - credential length
 * @param[in] credbuf - buffer to hold cred info
 *
 * @return 	string
 * @retval	jobid	success
 * @retval	NULL	error
 *
 */
char *
pbs_submit_with_cred(int c, struct attropl  *attrib, char *script, 
			char *destination, char  *extend, int credtype,
			size_t credlen, char  *credbuf)
{
	char					*ret;
	struct pbs_client_thread_context	*ptr;
	struct cred_info			*cred_info;

	/* initialize the thread context data, if not already initialized */
	if (pbs_client_thread_init_thread_context() != 0)
		return (char *)NULL;

	/* lock pthread mutex here for this connection */
	/* blocking call, waits for mutex release */
	if (pbs_client_thread_lock_connection(c) != 0)
		return (char *) NULL;

	ptr = (struct pbs_client_thread_context *)
		pbs_client_thread_get_context_data();
	if (!ptr) {
		pbs_errno = PBSE_INTERNAL;
		(void)pbs_client_thread_unlock_connection(c);
		return (char *) NULL;
	}

	if (!ptr->th_cred_info) {
		cred_info = malloc(sizeof(struct cred_info));
		if (!cred_info) {
			pbs_errno = PBSE_INTERNAL;
			(void)pbs_client_thread_unlock_connection(c);
			return (char *) NULL;
		}
		ptr->th_cred_info = (void *) cred_info;
	} else
		cred_info = (struct cred_info *) ptr->th_cred_info;

	/* copy credentials to static variables */
	cred_info->cred_buf = credbuf;
	cred_info->cred_len = credlen;
	cred_info->cred_type = credtype;

	/* pbs_submit takes credentials from static variables */
	ret = pbs_submit(c, attrib, script, destination, extend);

	cred_info->cred_buf = NULL;
	cred_info->cred_len = 0;
	cred_info->cred_type = 0;

	/* unlock the thread lock and update the thread context data */
	if (pbs_client_thread_unlock_connection(c) != 0)
		return (char *) NULL;

	return ret;
}

/**
 * @brief
 *	-submit job request
 *
 * @param[in] c - communication handle
 * @param[in] attrib - ponter to attr list
 * @param[in] script - job script
 * @param[in] destination - host where job submitted
 * @param[in] extend - buffer to hold cred info
 *
 * @return      string
 * @retval      jobid   success
 * @retval      NULL    error
 *
 */
char *
pbs_submit(int c, struct attropl  *attrib, char *script, char *destination, char *extend)
{
	struct attropl		*pal;
	char			*return_jobid = (char *)NULL;
	int			rc;
	struct pbs_client_thread_context *ptr;
	struct cred_info	*cred_info = NULL;

	/* initialize the thread context data, if not already initialized */
	if (pbs_client_thread_init_thread_context() != 0)
		return return_jobid;

	ptr = (struct pbs_client_thread_context *)
		pbs_client_thread_get_context_data();
	if (!ptr) {
		pbs_errno = PBSE_INTERNAL;
		return return_jobid;
	}

	/* first verify the attributes, if verification is enabled */
	rc = pbs_verify_attributes(c, PBS_BATCH_QueueJob,
		MGR_OBJ_JOB, MGR_CMD_NONE, attrib);
	if (rc)
		return return_jobid;

	/* lock pthread mutex here for this connection */
	/* blocking call, waits for mutex release */
	if (pbs_client_thread_lock_connection(c) != 0)
		return return_jobid;

	/* first be sure that the script is readable if specified ... */

	if ((script != (char *)0) && (*script != '\0')) {
		if (access(script, R_OK) != 0) {
			pbs_errno = PBSE_BADSCRIPT;
			if ((connection[c].ch_errtxt = strdup("cannot access script file")) == NULL)
				pbs_errno = PBSE_SYSTEM;
			goto error;
		}
	}

	/* initiate the queueing of the job */

	for (pal = attrib; pal; pal = pal->next)
		pal->op = SET;		/* force operator to SET */

	/* Queue job with null string for job id */
	return_jobid = PBSD_queuejob(c, "", destination, attrib, extend, 0, NULL);
	if (return_jobid == (char *)NULL)
		goto error;

	/* send script across */

	if ((script != (char *)0) && (*script != '\0')) {
		if ((rc = PBSD_jscript(c, script, 0, NULL)) != 0) {
			if (rc == PBSE_JOBSCRIPTMAXSIZE)
				pbs_errno = rc;
			else
				pbs_errno = PBSE_BADSCRIPT;
			goto error;
		}
	}

	/* OK, the script got across, apparently, so we are */
	/* ready to commit 				    */

	cred_info = (struct cred_info *) ptr->th_cred_info;

	/* opaque information */
	if (cred_info && cred_info->cred_len > 0) {
		if (PBSD_jcred(c, cred_info->cred_type,
			cred_info->cred_buf,
			cred_info->cred_len, 0, NULL) != 0) {
			pbs_errno = PBSE_BADCRED;
			goto error;
		}
	}

	if (PBSD_commit(c, return_jobid, 0, NULL) != 0)
		goto error;

	/* unlock the thread lock and update the thread context data */
	if (pbs_client_thread_unlock_connection(c) != 0)
		return (char *) NULL;

	return return_jobid;
error:
	(void)pbs_client_thread_unlock_connection(c);
	return (char *)NULL;
}
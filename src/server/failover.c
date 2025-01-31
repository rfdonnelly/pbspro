/*
 * Copyright (C) 1994-2019 Altair Engineering, Inc.
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
 * WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS
 * FOR A PARTICULAR PURPOSE.
 * See the GNU Affero General Public License for more details.
 *
 * You should have received a copy of the GNU Affero General Public License
 * along with this program.  If not, see <http://www.gnu.org/licenses/>.
 *
 * Commercial License Information:
 *
 * For a copy of the commercial license terms and conditions,
 * go to: (http://www.pbspro.com/UserArea/agreement.html)
 * or contact the Altair Legal Department.
 *
 * Altair’s dual-license business model allows companies, individuals, and
 * organizations to create proprietary derivative works of PBS Pro and
 * distribute them - whether embedded or bundled with other software -
 * under a commercial license agreement.
 *
 * Use of Altair’s trademarks, including but not limited to "PBS™",
 * "PBS Professional®", and "PBS Pro™" and Altair’s logos is subject to Altair's
 * trademark licensing policies.
 *
 */
/**
 * @brief
 * failover.c	- Functions relating to the FailOver Requests
 *
 *	Included public functions are:
 *
 *	rel_handshake
 *	utimes
 *	primary_handshake
 *	secondary_handshake
 *	fo_shutdown_reply
 *	failover_send_shutdown
 *	close_secondary
 *	put_failover
 *	req_failover
 *	read_fo_request
 *	read_reg_reply
 *	alm_handler
 *	alt_conn
 *	takeover_from_secondary
 *	be_secondary
 *
 */
#include <pbs_config.h>   /* the master config generated by configure */

#include <sys/types.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <fcntl.h>
#include <stdio.h>
#include <utime.h>
#include "libpbs.h"
#include <signal.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#ifdef WIN32
#include "win.h"
#else
#include <sys/wait.h>
#endif /* WIN32 */
#include "server_limits.h"
#include "credential.h"
#include "attribute.h"
#include "server.h"
#include "batch_request.h"
#include "net_connect.h"
#include "work_task.h"
#include "pbs_error.h"
#include "log.h"
#include "pbs_nodes.h"
#include "svrfunc.h"
#include "dis.h"
#include "libsec.h"
#include "pbs_db.h"


/* used internal to this file only */
#define SECONDARY_STATE_noconn	-1	/* not connected to Primary */
#define SECONDARY_STATE_conn	 0	/* connect to Primary */
#define SECONDARY_STATE_regsent	 1	/* have sent register to Primary */
#define SECONDARY_STATE_handsk	 3	/* receiving regular handshakes  */
#define SECONDARY_STATE_nohsk	 4	/* handsakes have stopped comming */
#define SECONDARY_STATE_shutd	 5	/* told to shut down */
#define SECONDARY_STATE_takeov	 6	/* primary back up and taking over */
#define SECONDARY_STATE_inact    7	/* told to go inactive/idle */
#define SECONDARY_STATE_idle     8	/* idle until primary back up */

#define HANDSHAKE_TIME	5

/* Global Data Items: */

extern char         *msg_daemonname;
extern unsigned long hostidnum;
extern char	    *path_priv;
extern char	    *path_svrlive;
extern char	    *path_secondaryact;
extern unsigned int  pbs_server_port_dis;
extern time_t	     secondary_delay;
extern time_t	     time_now;
extern char	     server_host[];

extern struct connection *svr_conn;
extern struct batch_request *saved_takeover_req;

int	     pbs_failover_active = 0; /* indicates if Seconary is active */
/* Private data items */

static int    sec_sock             = -1;  /* used by Secondary */
static int    Secondary_connection = -1;  /* used by Primary,connection_handle*/
static int    Secondary_state      = SECONDARY_STATE_noconn;
static time_t hd_time;
static int    goidle_ack	   = 0;
static char   msg_takeover[] = "received takeover message from primary, going inactive";
static char   msg_regfailed[]= "Primary rejected attempt to register as Secondary";

/**
 * @brief
 * 		rel_handshake_reply - free the batch request used for a handshake
 *		Cannot use release_req() as we don't want the connection closed
 *
 * @see
 * 		primary_handshake
 * @param[in]	pwt - pointer to the work task entry which invoked the function.
 *
 * @return: none
 */
static void
rel_handshake(struct work_task *pwt)
{
	DBPRT(("Failover: rel_handshake\n"))
	free_br((struct batch_request *)pwt->wt_parm1);
}

/**
 * @brief
 * 		Anything old enough to not have utimes() needs this.
 * 		This function sets the file access and modification times of the file - path.
 *
 * @param[in]	path - file location
 * @param[in]	times - pointer to the timeval structure
 * 						which stores access time and modification time
 * @return 	int
 * @retval	0	- Successful
 * @retval	-1	- Failure
 */
#if	defined(_SX) || defined(WIN32)
int
utimes(const char *path, const struct timeval *times)
{
	struct utimbuf	utimar, *utp = NULL;

	if (times != NULL) {
		utimar.actime = times[0].tv_sec;
		utimar.modtime = times[1].tv_sec;
		utp = &utimar;
	}

	return (utime(path, utp));
}
#endif	/* _SX */

/**
 * @brief
 *		Do the handshake with secondary server to show that the primary
 *		is still alive.
 *
 * @par Functionality:
 *		Perform periodic handshake to indicate to the Secondary Server that
 *		we, the Primary Server, is alive.  The handshake consists of three
 *		separate communication channels:
 *		1. "Touching" the "svrlive" file (in PBS_HOME/server_priv);
 *	   	this is done whenever this function is called.
 *		2. If a Secondary Server has registered, a "handshake" message is sent
 *	   	over the persistent TCP connection to the Secondary.
 *		3. Also if a Secondary has registered,  the "secondary_active" file is
 *	   	status-ed to see if it exists;  this is created by the Secondary
 *	   	when it goes active.  If this file is present, the Primary will
 *	   	restart itself in an attempt to take backover.
 *		This function is called from pbsd_main.c when it initializing.  It will
 *		create a work-task to recall itself every HANDSHAKE_TIME (5) seconds.
 *
 * @see
 *		main
 *
 * @param[in]	pwt - pointer to the work task entry which invoked the function.
 *
 * @return: none
 */
void
primary_handshake(struct work_task *pwt)
{
	struct batch_request *preq;
	struct stat sb;

	/* touch svrlive file as an "I am alive" sign */

	(void)update_svrlive();


	/* if connection, send HandShake request to Secondary */

	if (Secondary_connection >= 0) {
		DBPRT(("Failover: sending handshake\n"))
		if ((preq = alloc_br(PBS_BATCH_FailOver)) != NULL) {
			preq->rq_ind.rq_failover = FAILOVER_HandShake;
			if (issue_Drequest(Secondary_connection, preq, rel_handshake, 0, 0) != 0) {
				close_conn(Secondary_connection);
				Secondary_connection = -2;
			}
		}

		/* see if Secondary has taken over even though we are up */

		if (stat(path_secondaryact , &sb) != -1) {
			server.sv_attr[(int)SRV_ATR_State].at_val.at_long = SV_STATE_SECIDLE;  /* cause myself to recycle */
			DBPRT(("Primary server found secondary active, restarting\n"))
		}
	}

	/* reset work_task to call this again */

	(void)set_task(WORK_Timed, time_now + HANDSHAKE_TIME,
		primary_handshake, NULL);

	return;
}

/**
 * @brief
 *		"Touch" the sverlive file (in PBS_HOME/server_priv) to indicate that
 *		the Secondary Server is active.
 *
 * @par Functionality:
 *		When starting up, the Primary Server will monitor the time of "svrlive"
 *		to see if the Secondary appears to be active and needs to be told to
 *		go inactive; see pbsd_main.c.
 *		This function is first called out of the main program (pbsd_main.c)
 *		when the Secondary becomes the active server.  It will create a
 *		work-task to recall itself every HANDSHAKE_TIME (5) seconds.
 *
 * @see
 *		main
 *
 * @param[in]	pwt - pointer to the work task entry which invoked the function.
 *
 * @return: none
 */
void
secondary_handshake(struct work_task *pwt)
{
	(void)update_svrlive();
	(void)set_task(WORK_Timed, time_now + HANDSHAKE_TIME,
		secondary_handshake, NULL);
}


/**
 * @brief
 *		Handles reply from Secondary for shutdown  or go inactive message
 *		Clears the SV_STATE_PRIMDLY bit from the internal Server state so
 *		the Primary can exit from the main loop.
 *
 * @see
 *		failover_send_shutdown
 *
 * @param[in]	pwt - pointer to the work task entry which invoked the function.
 *
 * @return: none
 */
static void
fo_shutdown_reply(struct work_task *pwt)
{
	server.sv_attr[(int)SRV_ATR_State].at_val.at_long &= ~SV_STATE_PRIMDLY;
	release_req(pwt);
}
/**
 * @brief
 * 		Send a "shutdown" or "stay idle" request to the
 *		Secondary Server when the Primary is shutting down.
 *		Will cause a wait for the reply as this is critical, see
 *		fo_shutdown_reply().
 *
 * @see
 * 		svr_shutdown and req_shutdown
 *
 * @param[in] type	- type of message to send to Secondary
 *
 * @return  int - success or failure
 * @retval    0 - shutdown request sent to Secondary
 * @retval    1 - no Secondary connect, nothing (need be) done; or failed
 *		  to send message.
 */
int
failover_send_shutdown(int type)
{
	struct batch_request *preq;

	if (Secondary_connection == -1)
		return (1);	/* no secondary, nothing to do */

	if ((preq = alloc_br(PBS_BATCH_FailOver)) == NULL) {
		close_conn(Secondary_connection);
		Secondary_connection = -2;
		return (1);
	}
	preq->rq_ind.rq_failover = type;
	if (issue_Drequest(Secondary_connection, preq, fo_shutdown_reply, 0, 0)!=0) {
		close_conn(Secondary_connection);
		Secondary_connection = -2;
		return (1);
	}
	return (0);
}

/**
 * @brief
 * close_secondary - clear the Secondary_connect indictor when socket closed
 *
 * @see
 * 		req_failover
 *
 * @param[in] sock	- socket of connection
 *
 * @return  none
 */

static void
close_secondary(int sock)
{
	conn_t *conn;

	conn = get_conn(sock);
	if (!conn)
		return;

	if (Secondary_connection == conn->cn_handle)
		Secondary_connection = -1;

	DBPRT(("Failover: close secondary on socket %d\n", sock))

	return;
}

/**
 * @brief
 * 		put_failover - encode the FailOver request
 *		Used via issue_Drequest() by the active server for handshake/control
 *		used directly by the secondary for register message
 *
 *	@see
 *		takeover_from_secondary, be_secondary and issue_Drequest
 *
 * @param[in] sock	- socket of connection
 * @param[in] request	- FailOver request
 *
 * @return  int
 * @retval	0	- success
 * @retval	non-zero - decode failure error from a DIS routine
 */
int
put_failover(int sock, struct batch_request *request)
{
	int rc;


	DBPRT(("Failover: sending FO(%d) request\n", request->rq_ind.rq_failover))
	DIS_tcp_setup(sock);
	if ((rc = encode_DIS_ReqHdr(sock, PBS_BATCH_FailOver, pbs_current_user))==0)
		if ((rc = diswui(sock, request->rq_ind.rq_failover)) == 0)
			if ((rc=encode_DIS_ReqExtend(sock, 0)) == 0)
				rc = DIS_tcp_wflush(sock);
	return rc;
}


/**
 * @brief
 * 		req_failover - service failover related requests
 *		Primary - when receives "register", this is reached via process_request
 *		  and dispatch_request (like any request)
 *		Secondary - handshake and control calls, reached directly from
 *		  handshake_decode from be_secondary
 *
 * @see
 * 		dispatch_request and read_fo_request
 *
 * @param[in,out] preq	- pointer to failover request
 *
 * @return  none
 */

void
req_failover(struct batch_request *preq)
{
	int		 err = 0;
	char		 hostbuf[PBS_MAXHOSTNAME+1];
	conn_t		 *conn;

	preq->rq_reply.brp_auxcode = 0;

	conn = get_conn(preq->rq_conn);
	if (!conn) {
		req_reject(PBSE_SYSTEM, 0, preq);
		return;
	}

	DBPRT(("Failover: received FO(%d) request\n", preq->rq_ind.rq_failover))
	switch (preq->rq_ind.rq_failover) {

		case FAILOVER_Register:
			/*
			 * The one request that should be seen by the primary server
			 * - register the secondary and	return the primary's hostid
			 *
			 * Request must be from Secondary system with privileged
			 * for now - should be only one, so error if already one
			 */

			hostbuf[0] = '\0';
			(void)get_connecthost(preq->rq_conn, hostbuf, (sizeof(hostbuf) - 1));

			if (Secondary_connection >= 0) {
				err = PBSE_OBJBUSY;
				sprintf(log_buffer, "Failover: second secondary tried to register, host: %s", hostbuf);
				DBPRT(("%s\n", log_buffer))
				log_event(PBSEVENT_SYSTEM, PBS_EVENTCLASS_SERVER,
					LOG_WARNING, msg_daemonname, log_buffer);
				break;
			}
			sprintf(log_buffer, "Failover: registering %s as Secondary Server", hostbuf);
			DBPRT(("%s\n", log_buffer))
			log_event(PBSEVENT_SYSTEM, PBS_EVENTCLASS_SERVER,
				LOG_INFO, msg_daemonname, log_buffer);

			/* Mark the connection as non-expiring */

			conn->cn_authen |= PBS_NET_CONN_NOTIMEOUT;

			Secondary_connection = socket_to_handle(preq->rq_conn);
			conn->cn_func = process_Dreply;
			net_add_close_func(preq->rq_conn, close_secondary);

			/* return the host id as a text string */
			/* (make do with existing capability to return data in reply */

			sprintf(hostbuf, "%ld", hostidnum);
			(void)reply_text(preq, PBSE_NONE, hostbuf);
			return;

			/*
			 * the reminder of the requests come from the Primary to Secondary.
			 */

		case FAILOVER_HandShake:
			/* Handshake - the Primary is up, all is right with the world */
			/* record time of the handshake	,  then just acknowledge it   */
			hd_time = time(0);
			if (Secondary_state == SECONDARY_STATE_nohsk)
				Secondary_state = SECONDARY_STATE_handsk;
			break;

		case FAILOVER_PrimIsBack:
			/*
			 * Primary is Back - The Primary Server is back up and
			 * wishes to take control again.
			 * This is the only failover request normally
			 * seen by the Secondary while it is active.
			 */
			server.sv_attr[(int)SRV_ATR_State].at_val.at_long = SV_STATE_SECIDLE;
			log_event(PBSEVENT_SYSTEM, PBS_EVENTCLASS_SERVER, LOG_CRIT,
				msg_daemonname, msg_takeover);
			(void)unlink(path_secondaryact); /* remove file */
			DBPRT(("%s\n", msg_takeover))
			break;


			/* These requests come from the Primary while the 	*/
			/* Secondary is inactive 				*/

		case FAILOVER_SecdShutdown:
			/* Primary is shutting down, Secondary should also go down */
			log_event(PBSEVENT_SYSTEM, PBS_EVENTCLASS_SERVER, LOG_CRIT,
				msg_daemonname, "Failover: Secondary told to shut down");
#ifdef	WIN32
			log_err(0, "req_failover", "going down without auto_restart");
			make_server_auto_restart(0);
#endif
			reply_send(preq);
			exit(0);

		case FAILOVER_SecdGoInactive:
			/* Primary is shutting down, Secondary should remain inactive */
			Secondary_state = SECONDARY_STATE_inact;
			break;

		case FAILOVER_SecdTakeOver:
			sleep(10);	/* give primary a bit more time to go down */
			Secondary_state = SECONDARY_STATE_takeov;
			break;

		default:
			DBPRT(("Failover: invalid request\n"))
			err = 1;
	}


	if (err) {
		req_reject(PBSE_SYSTEM, 0, preq);
	} else {

		preq->rq_reply.brp_code = 0;
		preq->rq_reply.brp_choice = BATCH_REPLY_CHOICE_NULL;
		if (preq->rq_ind.rq_failover == FAILOVER_PrimIsBack) {
			/*
			 * save ptr of preq, Seconday will acknowledge the
			 * request after the nodes have been saved off
			 */
			saved_takeover_req = preq;
		} else if (preq->rq_ind.rq_failover == FAILOVER_SecdTakeOver) {
			/* acknowledge the request */
			reply_send(preq);
			/*
			 * Primary is shutting down, Secondary should go active
			 * wait for Primary to actually shut down
			 * (connection closes)
			 */
			(void)wait_request(600, NULL);
			if (sec_sock != -1) {
				close_conn(sec_sock);
				sec_sock = -1;
			}
		} else {
			/* acknowledge the request */
			DBPRT(("Failover: acknowledging FO(%d) request\n", preq->rq_ind.rq_failover))
			reply_send(preq);
		}
	}
	return;
}

/**
 * @brief
 *		Read and decode the failover request.  This function is
 *		used only by the secondary,  in place of process_request().
 *
 * @see
 * 		read_reg_reply
 *
 * @param[in] conn - connection on which the batch request is to be read
 *
 * @return None
 */
static void
read_fo_request(int conn)
{
	int                   rc;
	struct batch_request *request;

	if ((request = alloc_br(0)) == NULL) {		/* freed when reply sent */
		DBPRT(("Failover: Unable to allocate request structure\n"))
		Secondary_state = SECONDARY_STATE_noconn;
		close_conn(conn);
		sec_sock = -1;
		return;
	}
	request->rq_conn = conn;
	rc = dis_request_read(conn, request);
	DBPRT(("Failover: received request (rc=%d) secondary state %d\n", rc, Secondary_state))
	if (rc == -1) {
		/*
		 * EOF/socket closed, if the Secondary state is
		 * SECONDARY_STATE_inact or SECONDARY_STATE_noconn, then leave
		 * unchanged as there is a race as to when this end sees the
		 * connect closed by the primaryr;
		 * otherwise set to SECONDARY_STATE_nohsk to start timing
		 * to go active
		 */
		if ((Secondary_state != SECONDARY_STATE_inact) &&
			(Secondary_state != SECONDARY_STATE_noconn))
			Secondary_state = SECONDARY_STATE_nohsk;

		/* make sure our side is closed */
		close_conn(conn);
		sec_sock = -1;
		free_br(request);
		return;

	} else if (rc != 0) {
		/* read or decode error */
		DBPRT(("Failover: read or decode error\n"))
		Secondary_state = SECONDARY_STATE_noconn;
		close_conn(conn);
		sec_sock = -1;
		free_br(request);
		return;
	}

	req_failover(request);	/* will send reply, which will free request */
	return;
}

/**
 * @brief
 * 		read_reg_reply - read and decode the reply for one of two special failover
 *		messages: from the primary for the register request; or from the
 *		Secondary in reply to a take-over message.
 *
 * @par functionality:
 *		Normally, the active Server uses the normal process_Dreply() to
 *		read and decode the response to a message, even the normal failover
 *		messages such as handshake.  This function is used by the Secondary
 *		server only for the reply from register message.  On receiving a
 *		non-error reply, we advance the secondary state to "waiting for
 *		handshake".  If the Primary sends an explicit error (reject), the
 *		Secondary just exits as it isn't wanted.  Likewise on a read error,
 *		unless it is a EOF on the read.  In that case we assume the Primary
 *		really isn't up, and change set to "take over" which causes a retry
 * 		of the connection.
 *
 *		The Primary Server will use this to read/process the reply from a
 *		takeover message since at that point the Primary is not fully
 *		initialized.
 *
 * @see
 * 		takeover_from_secondary and be_secondary
 *
 * @param[in] sock - the socket from which to read.
 *
 * @return none
 */
static void
read_reg_reply(int sock)
{
	struct batch_reply fo_reply;
	int		   rc;
	unsigned long	   hid;
	char		  *txtm;
	char		  *txts;
	extern unsigned long pbs_get_hostid(void);

	fo_reply.brp_code = 0;
	fo_reply.brp_choice = BATCH_REPLY_CHOICE_NULL;
	fo_reply.brp_un.brp_txt.brp_txtlen = 0;
	fo_reply.brp_un.brp_txt.brp_str    = 0;
	rc = DIS_reply_read(sock, &fo_reply, 0);

	if ((rc != 0) || (fo_reply.brp_code != 0)) {
		DBPRT(("Failover: received invalid reply: non-zero code or EOF\n"))
		if ((rc == DIS_EOD) && (Secondary_state == SECONDARY_STATE_regsent)) {
			/* EOD/EOF on read of reply to register message,  */
			/* go ahead and take over as primary must be down */
			/* as we had successfully connected		  */
			Secondary_state = SECONDARY_STATE_takeov;
			return;
		}

		if (goidle_ack) {
			txts = pbs_conf.pbs_secondary;
			txtm = "failed to acknowledge request to go idle";
		} else {
			txts = pbs_conf.pbs_primary;
			txtm = "did not accept secondary registration";
		}
		sprintf(log_buffer, "Active PBS Server at %s %s", txts, txtm);
		log_event(PBSEVENT_SYSTEM, PBS_EVENTCLASS_SERVER, LOG_CRIT,
			msg_daemonname, log_buffer);
		exit(1);	/* bad reply */
	}

	if (goidle_ack) {
		/* waiting for reply to "go idle" request to active secondary */
		/* ok response means the active as agreed to shut down	  */
		goidle_ack = 0;	     /* see takeover_from_secondary() 	  */

	} else {

		if ((fo_reply.brp_choice != BATCH_REPLY_CHOICE_Text) ||
			(fo_reply.brp_un.brp_txt.brp_str == 0)) {

			if (fo_reply.brp_code == PBSE_UNKREQ) {
				log_event(PBSEVENT_SYSTEM, PBS_EVENTCLASS_SERVER, LOG_CRIT,
					msg_daemonname, msg_regfailed);
				DBPRT(("%s\n", msg_regfailed))
				exit(1);
			}
			DBPRT(("Failover: received invalid reply\n"))
			/* reset back to beginning */
			Secondary_state = SECONDARY_STATE_noconn;
		} else {

			char   fn[MAXPATHLEN+1];
			int    fd;
			size_t s;
			conn_t    *conn;

			conn = get_conn(sock);
			if (!conn) {
				log_event(PBSEVENT_SYSTEM, PBS_EVENTCLASS_SERVER,
					LOG_CRIT, msg_daemonname,
					"unable to socket in connection table");
				exit(1);
			}

			DBPRT(("Failover: received ok reply\n"))
			hid = (unsigned long)atol(fo_reply.brp_un.brp_txt.brp_str);
			hid = hid ^ pbs_get_hostid();
			free(fo_reply.brp_un.brp_txt.brp_str);
			fo_reply.brp_choice = BATCH_REPLY_CHOICE_NULL;
			/* change function for reading socket from read_reg_reply to */
			/* read_fo_request, and then wait for the handshakes	     */

			conn->cn_func = read_fo_request;
			Secondary_state = SECONDARY_STATE_handsk;
			hd_time = time(0);
			(void)sprintf(fn, "%s/license.fo", path_priv);
			/* save Primary's host id */
			fd = open(fn, O_WRONLY|O_CREAT|O_TRUNC, 0600);
			s = sizeof(hid);
			if ((fd == -1) || (write(fd, (char *)&hid, s) != s)) {
				log_event(PBSEVENT_SYSTEM, PBS_EVENTCLASS_SERVER, LOG_CRIT,
					msg_daemonname, "unable to save Primary hostid");
				exit(1);

			}
			close(fd);
		}
	}
}

/**
 * @brief
 * 		alm_handler - handler for sigalarm for alt_conn()
 *
 * @see
 * 		alt_conn
 *
 * @param[in] sig - signal signum.
 *
 * @return	none
 */
static void
alm_handler(int sig)
{
	return;
}

/**
 * @brief
 * alt_conn - connect to primary/secondary with timeout around the connect
 *
 * @see
 * 		takeover_from_secondary and be_secondary
 *
 * @param[in] addr - host address of primary or secondary.
 * @param[in] sec - timeout in seconds.
 *
 * @return	the socket from which to read
 * @retval	-1	- error
 */
static int
alt_conn(pbs_net_t addr, unsigned int sec)
{
	int sock;
	int mode;

#ifdef WIN32
	set_client_to_svr_timeout(sec);
	sock = client_to_svr(addr, pbs_server_port_dis, 1);
	set_client_to_svr_timeout(5);
	if (sock < 0)
		sock = -1;
#else
	struct sigaction act;

	act.sa_handler = alm_handler;
	sigemptyset(&act.sa_mask);
	act.sa_flags = 0;
	sigaction(SIGALRM, &act, 0);
	alarm(sec);
	mode = B_RESERVED;
	if (pbs_conf.auth_method == AUTH_MUNGE)
		mode = B_EXTERNAL|B_SVR;
	sock = client_to_svr(addr, pbs_server_port_dis, mode);
	alarm(0);
	if (sock < 0)
		sock = -1;
	act.sa_handler = SIG_DFL;
	sigaction(SIGALRM, &act, 0);
#endif

	return (sock);
}

/**
 * @brief
 * 	Function to check if stonith script exists at PBS_HOME/server_priv/stonith.
 * 	If it does then invoke the script for execution.
 * 
 * @param[in]	node - hostname of the node, that needs to brought down.
 * 
 * @return	Error code
 * @retval	 0 - stonith script executed successfully or script does not exist.
 * @retval      -1 - stonith script failed to bring down node.
 *
 */
int
check_and_invoke_stonith(char *node)
{
	char		stonith_cmd[3*MAXPATHLEN+1] = {0};
	char		stonith_fl[MAXPATHLEN+1] = {0};
	char		*p = NULL;
	char		out_err_fl[MAXPATHLEN+1] = {0};
	char		*out_err_msg = NULL;
	int		rc = 0;
	int		fd = 0;
	struct stat	stbuf;
	
	if (node == NULL )
		return -1;

	snprintf(stonith_fl, sizeof(stonith_fl), "%s/server_priv/stonith", pbs_conf.pbs_home_path);

#ifdef WIN32
	repl_slash(stonith_fl);
#endif /* WIN32 */

	if (stat(stonith_fl, &stbuf) != 0) {
		if (errno == ENOENT) {
			snprintf(log_buffer, LOG_BUF_SIZE, "Skipping STONITH");
			log_event(PBSEVENT_SYSTEM, PBS_EVENTCLASS_SERVER, LOG_INFO,
				  msg_daemonname, log_buffer);
			return 0;
		}
	}

	/* create unique filename by appending pid */
	snprintf(out_err_fl, sizeof(out_err_fl), 
		"%s/spool/stonith_out_err_fl_%s_%d", pbs_conf.pbs_home_path, node, getpid());
	
#ifdef WIN32
	repl_slash(out_err_fl);
#endif /* WIN32 */

	/* execute stonith script and redirect output to file */
	snprintf(stonith_cmd, sizeof(stonith_cmd), "%s %s > %s 2>&1", stonith_fl, node, out_err_fl);
	snprintf(log_buffer, LOG_BUF_SIZE, 
		"Executing STONITH script to bring down primary at %s", pbs_conf.pbs_server_name);
	log_event(PBSEVENT_SYSTEM, PBS_EVENTCLASS_SERVER, LOG_NOTICE,
			msg_daemonname, log_buffer);

#ifdef WIN32
	rc = wsystem(stonith_cmd, INVALID_HANDLE_VALUE);
#else 
	rc = system(stonith_cmd);
#endif /* WIN32 */

	if (rc != 0) {
		snprintf(log_buffer, LOG_BUF_SIZE, 
			"STONITH script execution failed, script exit code: %d", rc);
		log_event(PBSEVENT_ERROR, PBS_EVENTCLASS_SERVER, LOG_CRIT,
			msg_daemonname, log_buffer);
	} else {
		snprintf(log_buffer, LOG_BUF_SIZE, 
			"STONITH script executed successfully");
		log_event(PBSEVENT_SYSTEM, PBS_EVENTCLASS_SERVER, LOG_INFO,
			  msg_daemonname, log_buffer);
	}

	/* read the contents of out_err_fl and load to out_err_msg */
	if ((fd = open(out_err_fl, 0)) != -1) {
		if (fstat(fd, &stbuf) != -1) {
			out_err_msg = malloc(stbuf.st_size + 1);
			if (out_err_msg == NULL) {
				close(fd);
				unlink(out_err_fl);
				log_err(errno, __func__, "malloc failed");
				return -1;
			}

			if (read(fd, out_err_msg, stbuf.st_size) == -1) {
				close(fd);
				snprintf(log_buffer, LOG_BUF_SIZE, 
					"%s: read failed, errno: %d", out_err_fl, errno);
				log_err(errno, __func__, log_buffer);
				free(out_err_msg);
				return -1;
			}

			*(out_err_msg + stbuf.st_size) = '\0';
			p = out_err_msg + strlen(out_err_msg) - 1;

			while ((p >= out_err_msg) && (*p == '\r' || *p == '\n'))
				*p-- = '\0'; /* supress the last newline */
		}
		close(fd);
	}

	if (out_err_msg) {
		snprintf(log_buffer, LOG_BUF_SIZE, 
			"%s, exit_code: %d.", out_err_msg, rc);
		log_event(PBSEVENT_SYSTEM, PBS_EVENTCLASS_SERVER, LOG_INFO, 
			msg_daemonname, log_buffer);
		free(out_err_msg);
	}

	unlink(out_err_fl);

	if (rc != 0)
		return -1;
	return rc;
}

/**
 * @brief
 *		Take control back from an active Secondary Server
 *
 * @par Functionality:
 *		Attempt to connect to the Secondary Server,  timeout the connection
 *		request if it isn't accepted in a short time.  If the connection
 *		cannot be made because the IP address is not available or if the
 *		connection is made but the Secondary does not acknowledge the request,
 *		the the Primary will print a message and exit (the process).
 *
 * @see
 * 		main
 *
 * @return  - int: failover server role
 * @retval  0 - unable to contact the Secondary Server.
 * @retval  1 - Contacted Secondary and it acknowledged the takeover request.
 */
int
takeover_from_secondary()
{
	struct batch_request	*pfo_req;
	pbs_net_t		 addr;
	int			 sock;
	conn_t 			 *conn;

	/*
	 * need to do a limited initialization of the network tables,
	 * connect to the secondary,
	 * send a go away message,
	 * wait for the reply which is very ununusual for us,
	 * wait a bit, and then clean up the network tables
	 */

	(void)init_network(0);
	(void)init_network_add(-1, NULL, NULL);

	/* connect to active secondary if we can */
	/* if connected, send take-over request */
	/* wait for reply */
	addr = get_hostaddr(pbs_conf.pbs_secondary);
	if (addr == (pbs_net_t)0) {
		fprintf(stderr, "Cannot get network address of Secondary, aborting\n");
		exit(1);
	}
	sock = alt_conn(addr, 4);
	if (sock < 0)
		return 0;

	conn = add_conn(sock, ToServerDIS, addr, 0, NULL, read_reg_reply);
	if (conn == NULL) {
		/* path highly unlikely but theoretically possible */

		fprintf(stderr, "Connection not found, abort takeover from secondary\n");
		exit(1);
	}

	conn->cn_authen |= PBS_NET_CONN_AUTHENTICATED;

	if ((pfo_req = alloc_br(PBS_BATCH_FailOver)) == NULL) {
		fprintf(stderr, "Unable to allocate request structure, abort takeover from secondary\n");
		exit(1);
	}
	pfo_req->rq_ind.rq_failover = FAILOVER_PrimIsBack;
	if (put_failover(sock, pfo_req) != 0) {
		fprintf(stderr, "Could not communicate with Secondary, aborting\n");
		exit(1);
	}
	goidle_ack = 1;
	(void)wait_request(600, NULL);
#ifdef WIN32
	connection_idlecheck();
#endif
	if (goidle_ack == 1) {
		/* cannot seem to force active secondary to go idle */
		fprintf(stderr, "Secondary not idling, aborting\n");
		exit(2);
	}
	printf("Have taken control from Secondary Server\n");
	return 1;
}

/**
 * @brief
 * 		be_secondary - detect if primary is up
 *		if primary up - wait for it to go down, then take over
 *		if primary down and delay is not -1, wait for primary to come up
 *		if primary down and delay is -1, then take over now
 *
 * @see
 *		main
 *
 * @param[in] delay - time for which secondary to wait for primary to come up.
 *
 *	returns: 1 - if should stay inactive secondary
 *		 0 - if should take over as active
 */
int
be_secondary(time_t delay)
{
	int			 loop = 0;
	pbs_net_t		 primaddr;
	struct batch_request	*pfo_req;
	struct stat		 sb;
	int			 sbloop = 0;
	time_t			 sbtime = 0;
	FILE			*secact;
	time_t			 mytime = 0;
	time_t			 takeov_on_nocontact;
	conn_t			 *conn;
	int			 mode;
	int			 rc = 0;

	/*
	 * do limited initialization of the network tables
	 * send register request to Primary
	 * loop waiting for handshake
	 */

	(void)init_network(0);
	(void)init_network_add(-1, NULL, NULL);
	hd_time = time(0);

	/* connect to primary */

	primaddr = get_hostaddr(pbs_conf.pbs_primary);
	if (primaddr == (pbs_net_t)0) {
		fprintf(stderr, "pbs_server: unable to obtain Primary Server's network address, aborting.\n");
		exit(1);
	}

	if (secondary_delay == (time_t)-1) {
		secondary_delay = 0;
		printf("pbs_server: secondary directed to start up as active\n");
	} else {
		sprintf(log_buffer, "pbs_server: coming up as Secondary, Primary is %s", pbs_conf.pbs_primary);
		printf("%s\n", log_buffer);
		log_event(PBSEVENT_SYSTEM, PBS_EVENTCLASS_SERVER, LOG_NOTICE,
			msg_daemonname, log_buffer);

	}
	takeov_on_nocontact = hd_time + (60 * 5) + secondary_delay;

#ifndef DEBUG
	pbs_close_stdfiles();	/* set stdin/stdout/stderr to /dev/null */
#endif	/* DEBUG */

	/*
	 * Secondary Server State machine
	 */

	while (1) {
		time_now = time(0);
		++loop;

		DBPRT(("Failover: Secondary_state is %d\n", Secondary_state));
		switch (Secondary_state) {
			case SECONDARY_STATE_noconn:
			case SECONDARY_STATE_idle:

				/* for both noconn and idle try to reconnect */
				sbloop = 0;
				sbtime = 0;
				mytime = 0;

				if (sec_sock >= 0)
					close_conn(sec_sock);

				mode = B_RESERVED;
				if (pbs_conf.auth_method == AUTH_MUNGE)
					mode = B_EXTERNAL|B_SVR;

				sec_sock = client_to_svr(primaddr, pbs_server_port_dis, mode);
				if (sec_sock < 0) {

					/* failed to reconnect to primary */
					/* if _idle, just try again later */
					/* else if time is up, go active  */

					if ((Secondary_state == SECONDARY_STATE_noconn) && ((delay == (time_t)-1) || (time_now > takeov_on_nocontact))) {
						/* can take over role of active server */
						sec_sock = -1;
						Secondary_state = SECONDARY_STATE_takeov;
					} else {
						/* wait for primary to come up and try again */
						sec_sock = -1;
						sleep(10);
					}
				} else {
					/* made contact with primary, set to send registration */
					Secondary_state = SECONDARY_STATE_conn;
					conn = add_conn(sec_sock, ToServerDIS, primaddr, 0, NULL,
						read_reg_reply);
					if (conn) {
						conn->cn_authen |= PBS_NET_CONN_AUTHENTICATED;
						DBPRT(("Failover: reconnected to primary\n"))
					} else {
						/* a possible but unlikely case */
						log_err(-1, "be_secondary",
							"Connection not found, close socket free context");
						(void)CS_close_socket(sec_sock);
						close(sec_sock);
					}
				}
				break;

			case SECONDARY_STATE_conn:

				/* Primary is up, so send register request and wait on reply */
				/* state changed when reply processed, see read_reg_reply     */

				if ((pfo_req = alloc_br(PBS_BATCH_FailOver)) == NULL) {
					close_conn(sec_sock);
					Secondary_state = SECONDARY_STATE_noconn;
					sec_sock = -1;
				} else {
					pfo_req->rq_ind.rq_failover = FAILOVER_Register;
					if (put_failover(sec_sock, pfo_req) == 0) {
						Secondary_state = SECONDARY_STATE_regsent;
					} else {
						close_conn(sec_sock);
						Secondary_state = SECONDARY_STATE_noconn;
						sec_sock = -1;
					}
					free_br(pfo_req);
				}
				break;

			case SECONDARY_STATE_regsent:
				/* waiting on reply from register, do nothing ... */
				/* state will change in read_reg_reply() 	  */
				break;

			case SECONDARY_STATE_handsk:
				/* waiting for handshake from the primary	*/
				/* check to see if it has been too long		*/
				if (time_now >= (hd_time + 2 * HANDSHAKE_TIME)) {
					/* haven't received handshake recently */
					Secondary_state = SECONDARY_STATE_nohsk;
					sprintf(log_buffer, "Secondary has not received handshake in %ld seconds", time_now - hd_time);
					log_event(PBSEVENT_SYSTEM, PBS_EVENTCLASS_SERVER,
						LOG_WARNING, msg_daemonname, log_buffer);
				}
				break;

			case SECONDARY_STATE_nohsk:
				/* have not received a hankshake or connection closed */
				/* check time stamp on path_svrdb */
				if (stat(path_svrlive, &sb) == 0) {

					/* able to stat the server database */

					DBPRT(("Failover: my: %ld stat: %ld dly: %ld\n",
						time_now, sb.st_mtime, secondary_delay))

					if (sb.st_mtime > sbtime) {
						/* mtime appears to be changing...           */
						/* this happens at least the first time here */
						sbtime = sb.st_mtime;
						mytime = time_now;

						if ((sbloop++ > 4) && (sec_sock == -1)) {
							/* files still being touched, but    */
							/* no handshake,  try to reconnect   */
							DBPRT(("Failover: going to noconn, still being touched\n"))
							Secondary_state = SECONDARY_STATE_noconn;
						}

					} else if (time_now > (mytime + secondary_delay)) {
						/* mtime hasn't changed in too long, take over */
						Secondary_state = SECONDARY_STATE_takeov;
					}

				} else if (time_now > (hd_time + secondary_delay)) {
					/*
					 * couldn't stat the directory in the last
					 * "secondary_delay" seconds, Secondary must be
					 * the one off the network, try to reconnect
					 */
					Secondary_state = SECONDARY_STATE_noconn;
					log_event(PBSEVENT_SYSTEM, PBS_EVENTCLASS_SERVER,
						LOG_CRIT, msg_daemonname,
						"Secondary unable to stat server live file");
					DBPRT(("Failover: going to noconn, cannot stat\n"))

				} else if ((sec_sock == -1) && ((loop % 3) == 0)) {
					/* no connection and cannot stat, try   */
					/* once in a while to quickly reconnect */
					if ((sec_sock = alt_conn(primaddr, 8)) >= 0) {
						Secondary_state = SECONDARY_STATE_conn;
						conn = add_conn(sec_sock, ToServerDIS, primaddr, 0, NULL,
							read_reg_reply);
						if (conn) {
							conn->cn_authen |= PBS_NET_CONN_AUTHENTICATED;
							DBPRT(("Failover: reconnected to primary\n"))
						} else {
							/* a possible but unlikely case */
							log_err(-1, "be_secondary",
								"Connection not found, close socket free context");
							(void)CS_close_socket(sec_sock);
							close(sec_sock);
						}
					}
				}
				break;

			case SECONDARY_STATE_shutd:
				exit(0);	/* told to shutdown */

			case SECONDARY_STATE_takeov:
				/* check with Primary one last time before taking over */
				if (sec_sock != -1) {
					close_conn(sec_sock);
					sec_sock = -1;
				}
				log_event(PBSEVENT_SYSTEM, PBS_EVENTCLASS_SERVER, LOG_NOTICE,
					msg_daemonname, "Secondary attempting to connect with Primary one last time before taking over");
				if ((sec_sock = alt_conn(primaddr, 8)) >= 0) {
					Secondary_state = SECONDARY_STATE_conn;
					conn = add_conn(sec_sock, ToServerDIS, primaddr, 0, NULL,
						read_reg_reply);
					if (conn) {
						conn->cn_authen |= PBS_NET_CONN_AUTHENTICATED;
						log_event(PBSEVENT_SYSTEM, PBS_EVENTCLASS_SERVER,
							LOG_NOTICE, msg_daemonname,
							"Secondary reconnected with Primary");
					} else {
						/* a possible but unlikely case */
						log_err(-1, "be_secondary",
							"Connection not found, close socket free context");
						(void)CS_close_socket(sec_sock);
						close(sec_sock);
					}
					break;
				}
				/* Invoke stonith, to make sure primary is down */
				rc = check_and_invoke_stonith(pbs_conf.pbs_primary);
				if (rc) {
					snprintf(log_buffer, LOG_BUF_SIZE, 
						"Secondary will attempt taking over again");
					log_event(PBSEVENT_SYSTEM, PBS_EVENTCLASS_SERVER, LOG_INFO,
						msg_daemonname, log_buffer);

					sleep(10);
					break;
				}
				/* take over from primary */
				pbs_failover_active = 1;
				secact = fopen(path_secondaryact, "w");
				if (secact != NULL) {
					/* create file that says secondary is up */
					fprintf(secact, "%s\n", server_host);
					fclose(secact);
					DBPRT(("Secondary server creating %s\n", path_secondaryact))
				}
				return (0);

			case SECONDARY_STATE_inact:
				/*
				 * first wait for Primary to close connection indicating
				 * that it is going down, then wait a safety few more seconds
				 */
				(void)wait_request(600, NULL);
				sleep(10);
				log_event(PBSEVENT_DEBUG, LOG_DEBUG, PBS_EVENTCLASS_SERVER,
					msg_daemonname,
					"Secondary completed waiting for Primary to go down");
				close_conn(sec_sock);
				sec_sock = -1;
				/* change state to indicate Secondary is idle */
				Secondary_state = SECONDARY_STATE_idle;
				/* will recycle back to the top */
				break;
		}

		if (wait_request(1, NULL) == -1) {
			Secondary_state = SECONDARY_STATE_noconn;
			close_conn(sec_sock);
			sec_sock = -1;
		}
#ifdef WIN32
		connection_idlecheck();
#endif
	}
}

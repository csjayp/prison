/*-
 * Copyright (c) 2020 Christian S.J. Peron
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 *
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the distribution.
 *
 * THIS SOFTWARE IS PROVIDED BY THE AUTHOR AND CONTRIBUTORS ``AS IS'' AND
 * ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
 * ARE DISCLAIMED.  IN NO EVENT SHALL THE AUTHOR OR CONTRIBUTORS BE LIABLE
 * FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL
 * DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS
 * OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION)
 * HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT
 * LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY
 * OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF
 * SUCH DAMAGE.
 */
#include <sys/types.h>
#include <sys/queue.h>
#include <sys/stat.h>
#include <sys/socket.h>
#include <sys/wait.h>
#include <sys/ioctl.h>
#include <sys/ttycom.h>

#include <stdio.h>
#include <paths.h>
#include <pthread.h>
#include <unistd.h>
#include <fcntl.h>
#include <err.h>
#include <errno.h>
#include <stdint.h>
#include <stdlib.h>
#include <termios.h>
#include <libutil.h>
#include <signal.h>
#include <assert.h>
#include <string.h>

#include <libprison.h>

#include <openssl/sha.h>

#include "termbuf.h"
#include "main.h"
#include "dispatch.h"
#include "sock_ipc.h"
#include "config.h"

TAILQ_HEAD( , prison_peer) p_head;
TAILQ_HEAD( , prison_instance) pr_head;

static int reap_children;

pthread_mutex_t peer_mutex;
pthread_mutex_t prison_mutex;

FILE *df;

static void
handle_reap_children(int sig)
{

	reap_children = 1;
}

void
prison_remove(struct prison_instance *pi)
{
	uint32_t cmd;
	size_t cur;
	vec_t *vec;
	char buf[128];
	int status, ret;
	/*
	 * Tell the remote side to dis-connect.
	 *
	 * NB: we are holding a lock here. We need to re-factor this a bit
	 * so we aren't performing socket io while this lock is held.
	 */
	if ((pi->p_state & STATE_CONNECTED) != 0) {
		cmd = PRISON_IPC_CONSOLE_SESSION_DONE;
		sock_ipc_must_write(pi->p_peer_sock, &cmd, sizeof(cmd));
	}
	(void) close(pi->p_peer_sock);
	(void) close(pi->p_ttyfd);
	termbuf_print_queue(&pi->p_ttybuf.t_head);
	TAILQ_REMOVE(&pr_head, pi, p_glue);
	cur = pi->p_ttybuf.t_tot_len;
	while (cur > 0) {
		cur = termbuf_remove_oldest(&pi->p_ttybuf);
	}
	vec = vec_init(16);
	vec_append(vec, "/bin/sh");
	sprintf(buf, "%s/lib/stage_launch_cleanup.sh", gcfg.c_data_dir);
	vec_append(vec, buf);
	vec_append(vec, gcfg.c_data_dir);
	vec_append(vec, pi->p_instance_tag);
	sprintf(buf, "%d", pi->p_type);
	vec_append(vec, buf);
	vec_finalize(vec);
	pid_t pid = fork();
	if (pid == -1) {
		err(1, "prison_remove: failed to execute cleanup handlers");
	}
	if (pid == 0) {
		char **argv;
		argv = vec_return(vec);
		execve(*argv, argv, NULL);
		err(1, "prison_remove: execve failed");
	}
	while (1) {
		ret = waitpid(pid, &status, 0);
		if (ret == -1 && errno == EINTR) {
			continue;
		}
		if (ret == -1) {
			err(1, "waitpid failed");
		}
		break;
	}
	vec_free(vec);
	free(pi);
}

static void
prison_detach_console(const char *name)
{
	struct prison_instance *pi;

	pthread_mutex_lock(&prison_mutex);
	TAILQ_FOREACH(pi, &pr_head, p_glue) {
		if (strcmp(pi->p_name, name) != 0) {
			continue;
		}
		pi->p_state &= ~STATE_CONNECTED;
		pi->p_peer_sock = -1;
		pthread_mutex_unlock(&prison_mutex);
		return;
	}
	pthread_mutex_unlock(&prison_mutex);
	/*
	 * If we are here, the process was non-interactive (build job) and
	 * has completed already.
	 */
}

static void
prison_reap_children(void)
{
	struct prison_instance *pi, *p_temp;
	int status;
	pid_t pid;

	pthread_mutex_lock(&prison_mutex);
	TAILQ_FOREACH_SAFE(pi, &pr_head, p_glue, p_temp) {
		pid = waitpid(pi->p_pid, &status, WNOHANG);
		if (pid != pi->p_pid) {
			continue;
		}
		pi->p_state |= STATE_DEAD;
		printf("collected exit status from proc %d\n", pi->p_pid);
		prison_remove(pi);
	}
	pthread_mutex_unlock(&prison_mutex);
	reap_children = 0;
}

static int
tty_initialize_fdset(fd_set *rfds)
{
	struct prison_instance *pi, *p_temp;
	int maxfd;

	FD_ZERO(rfds);
	maxfd = 0;
	pthread_mutex_lock(&prison_mutex);
	TAILQ_FOREACH_SAFE(pi, &pr_head, p_glue, p_temp) {
		if ((pi->p_state & STATE_DEAD) != 0) {
			continue;
		}
		if (pi->p_ttyfd > maxfd) {
			maxfd = pi->p_ttyfd;
		}
		FD_SET(pi->p_ttyfd, rfds);
	}
	pthread_mutex_unlock(&prison_mutex);
	return (maxfd);
}

void *
tty_io_queue_loop(void *arg)
{
	struct prison_instance *pi;
	struct timeval tv;
	u_char buf[8192];
	int maxfd, error;
	uint32_t cmd;
	fd_set rfds;
	ssize_t cc;
	size_t len;

	printf("tty_io_queue_loop: dispatched\n");
	while (1) {
		prison_reap_children();
		maxfd = tty_initialize_fdset(&rfds);
		tv.tv_sec = 0;
		tv.tv_usec = 500000;
		error = select(maxfd + 1, &rfds, NULL, NULL, &tv);
		if (error == -1 && errno == EINTR) {
			printf("select interrupted\n");
			continue;
		}
		if (error == -1) {
			err(1, "select(tty io) failed");
		}
		if (error == 0) {
			continue;
		}
		pthread_mutex_lock(&prison_mutex);
		TAILQ_FOREACH(pi, &pr_head, p_glue) {
			if (!FD_ISSET(pi->p_ttyfd, &rfds)) {
				continue;
			}
			cc = read(pi->p_ttyfd, buf, sizeof(buf));
			if (cc == 0) {
				printf("state dead for %s\n", pi->p_name);
				reap_children = 1;
				pi->p_state |= STATE_DEAD;
				continue;
			}
			if (cc == -1) {
				err(1, "%s: read failed:", __func__);
			}
			termbuf_append(&pi->p_ttybuf, buf, cc);
			if (pi->p_state != STATE_CONNECTED) {
				continue;
			}
			len = cc;
			cmd = PRISON_IPC_CONSOLE_TO_CLIENT;
			sock_ipc_must_write(pi->p_peer_sock, &cmd, sizeof(cmd));
			sock_ipc_must_write(pi->p_peer_sock, &len, sizeof(len));
			sock_ipc_must_write(pi->p_peer_sock, buf, cc);
		}
		pthread_mutex_unlock(&prison_mutex);
	}
}

static int
prison_instance_is_unique(const char *name)
{
	struct prison_instance *pi;

	pthread_mutex_lock(&prison_mutex);
	TAILQ_FOREACH(pi, &pr_head, p_glue) {
		if (strcmp(pi->p_name, name) == 0) {
			pthread_mutex_unlock(&prison_mutex);
			return (0);
		}
	}
	pthread_mutex_unlock(&prison_mutex);
	return (1);
}

static int
prison_instance_is_dead(const char *name)
{
	struct prison_instance *pi;
	int isdead;

	isdead = 0;
	pthread_mutex_lock(&prison_mutex);
	TAILQ_FOREACH(pi, &pr_head, p_glue) {
		if (strcmp(pi->p_name, name) != 0) {
			continue;
		}
		isdead = ((pi->p_state & STATE_DEAD) != 0);
		pthread_mutex_unlock(&prison_mutex);
		return (isdead);
        }
        pthread_mutex_unlock(&prison_mutex);
	/*
	 * The console was non-interactive (i.e.: a build job) and it is
	 * complete and the process(s) have been reaped. We might want
	 * to assert that this is the case some place.
	 */
        return (1);
}

void
dispatch_handle_resize(int ttyfd, char *buf)
{
	struct winsize *wsize;
	char *vptr;

	vptr = buf;
	vptr += sizeof(uint32_t);
	wsize = (struct winsize *)vptr;
	if (ioctl(ttyfd, TIOCSWINSZ, wsize) == -1) {
		err(1, "ioctl(TIOCSWINSZ): failed");
	}
}

void
tty_console_session(const char *name, int sock, int ttyfd)
{
	char buf[1024], *vptr;
	uint32_t *cmd;
	ssize_t bytes;

	printf("tty_console_session: enter, reading commands from client\n");
	for (;;) {
		bzero(buf, sizeof(buf));
		ssize_t cc = read(sock, buf, sizeof(buf));
		if (cc == 0) {
			printf("read EOF from socket\n");
			break;
		}
		if (cc == -1 && errno == EINTR) {
			continue;
		}
		if (cc == -1) {
			err(1, "%s: read failed", __func__);
		}
		/*
		 * NB: There probably needs to be a better way to do this 
		 * rather than iterating through the jail list for read input
		 * from the console.
		 */
		if (prison_instance_is_dead(name)) {
			break;
		}
		vptr = buf;
		cmd = (uint32_t *)vptr;
		switch (*cmd) {
		case PRISON_IPC_CONSOL_RESIZE:
			dispatch_handle_resize(ttyfd, buf);
			break;
		case PRISON_IPC_CONSOLE_DATA:
			vptr += sizeof(uint32_t);
			cc -= sizeof(uint32_t);
			bytes = write(ttyfd, vptr, cc);
			if (bytes != cc) {
				err(1, "tty_write failed");
			}
			break;
		default:
			errx(1, "unknown console instruction");
		}
	}
	printf("console dis-connected\n");
}

struct prison_instance *
prison_lookup_instance(const char *name)
{
	struct prison_instance *pi;

	TAILQ_FOREACH(pi, &pr_head, p_glue) {
		if (strcmp(name, pi->p_name) != 0) {
			continue;
		}
		return (pi);
	}
	return (NULL);
}

int
dispatch_connect_console(int sock)
{
	struct prison_console_connect pcc;
	struct prison_response resp;
	struct prison_instance *pi;
	ssize_t tty_buflen;
	char *tty_block;
	uint32_t cmd;
	size_t len;
	int ttyfd;

	bzero(&resp, sizeof(resp));
	sock_ipc_must_read(sock, &pcc, sizeof(pcc));
	pthread_mutex_lock(&prison_mutex);
	pi = prison_lookup_instance(pcc.p_name);
	if (pi == NULL) {
		pthread_mutex_unlock(&prison_mutex);
		sprintf(resp.p_errbuf, "%s invalid container", pcc.p_name);
		resp.p_ecode = 1;
		sock_ipc_must_write(sock, &resp, sizeof(resp));
		return (1);
	}
	if ((pi->p_state & STATE_CONNECTED) != 0) {
		pthread_mutex_unlock(&prison_mutex);
		sprintf(resp.p_errbuf, "%s console already attached", pcc.p_name);
		resp.p_ecode = 1;
		sock_ipc_must_write(sock, &resp, sizeof(resp));
		return (1);
	}
	pi->p_state = STATE_CONNECTED;
	ttyfd = pi->p_ttyfd;
	tty_block = termbuf_to_contig(&pi->p_ttybuf);
	tty_buflen = pi->p_ttybuf.t_tot_len;
	pi->p_peer_sock = sock;
	pthread_mutex_unlock(&prison_mutex);
	resp.p_ecode = 0;
	sock_ipc_must_write(sock, &resp, sizeof(resp));
	if (tty_block) {
		cmd = PRISON_IPC_CONSOLE_TO_CLIENT;
		sock_ipc_must_write(sock, &cmd, sizeof(cmd));
		len = tty_buflen;
		sock_ipc_must_write(sock, &len, sizeof(len));
		sock_ipc_must_write(sock, tty_block, tty_buflen);
		free(tty_block);
	}
	if (tcsetattr(ttyfd, TCSANOW, &pcc.p_termios) == -1) {
		err(1, "tcsetattr(TCSANOW) console connect");
	}
	if (ioctl(ttyfd, TIOCSWINSZ, &pcc.p_winsize) == -1) {
		err(1, "ioctl(TIOCSWINSZ): failed");
	}
	/*
	 * If this console connection is the result of a container build, the
	 * build process will be blocked waiting for the console connection.
	 * write a single byte to the pipe to trigger the execution.
	 *
	 * NB: instead of checking the file descriptor, we should be using a
	 * flag
	 */
	if (pi->p_pipe[1] != 0) {
		char b;
		printf("signaling to build process\n");
		write(pi->p_pipe[1], &b, 1);
	}
	tty_console_session(pcc.p_name, sock, ttyfd);
	prison_detach_console(pcc.p_name);
	return (1);
}

struct prison_instance *
prison_create(const char *name, char *term, int (*prison_callback)(void *, struct prison_instance *),
    void *arg)
{
	struct prison_instance *pi;
	int ret;

	if (!prison_instance_is_unique(name)) {
		return (NULL);
	}
	pi = calloc(1, sizeof(*pi));
	if (pi == NULL) {
		return (NULL);
	}
	pi->p_type = PRISON_TYPE_BUILD;
	strlcpy(pi->p_name, name, sizeof(pi->p_name));
	if (pipe(pi->p_pipe) == -1) {
		warn("pipe failed");
		return (NULL);
	}
	pi->p_pid = forkpty(&pi->p_ttyfd, pi->p_ttyname, NULL, NULL);
	if (pi->p_pid == 0) {
		printf("\n");
		ssize_t cc;
		char b;
		close(pi->p_pipe[1]);
		while (1) {
			printf("waiting for console synchronization\n");
			cc = read(pi->p_pipe[0], &b, 1);
			if (cc == -1 && errno == EINTR)
				continue;
			if (cc == -1)
				err(1, "%s: read failed", __func__);
			break;
			assert(cc == 1);
		}
		printf("wokeup, continuing...\n");
		if (setenv("TERM", term, 1) != 0) {
			err(1, "setenv failed");
		}
		ret = (prison_callback)(arg, pi);
		_exit(ret);
	}
	printf("DEBUG: TTY name %s\n", pi->p_ttyname);
	close(pi->p_pipe[0]);
	TAILQ_INIT(&pi->p_ttybuf.t_head);
	pi->p_ttybuf.t_tot_len = 0;
	return (pi);
}

int
dispatch_build_launch(int sock)
{
	struct prison_build_context pbc;
	struct prison_response resp;
	struct build_context *bcp;
	char prison_name[32];
	ssize_t cc;
	struct prison_instance *pi;

	cc = sock_ipc_must_read(sock, &pbc, sizeof(pbc));
	bcp = build_lookup_queued_context(&pbc);
	if (bcp == NULL) {
		resp.p_ecode = ENOENT;
		sock_ipc_must_write(sock, &resp, sizeof(resp));
		return (-1);
	}
	snprintf(prison_name, sizeof(prison_name), "%s:%s",
	    pbc.p_image_name, pbc.p_tag);
	pi = prison_create(prison_name, pbc.p_term, do_build_launch, bcp);
	if (pi == NULL) {
		resp.p_ecode = -1;
		sock_ipc_must_write(sock, &resp, sizeof(resp));
		return (-1);
	}
	pi->p_instance_tag = bcp->instance;
	pthread_mutex_lock(&prison_mutex);
	TAILQ_INSERT_HEAD(&pr_head, pi, p_glue);
	pthread_mutex_unlock(&prison_mutex);
	resp.p_ecode = 0;
	resp.p_errbuf[0] = '\0';
	sock_ipc_must_write(sock, &resp, sizeof(resp));
	return (1);
}

void
gen_sha256_string(unsigned char *hash, char *output)
{
	int k;

	for (k = 0; k < SHA256_DIGEST_LENGTH; k++) {
		sprintf(output + (k * 2), "%02x", hash[k]);
	}
	output[64] = '\0';
}

char *
gen_sha256_instance_id(char *instance_name)
{
	u_char hash[SHA256_DIGEST_LENGTH];
	struct timeval tv;
	char inbuf[128];
	char outbuf[128+1];
	SHA256_CTX sha256;

	bzero(outbuf, sizeof(outbuf));
	gettimeofday(&tv, NULL);
	snprintf(inbuf, sizeof(inbuf), "%lu:%lu:%s", tv.tv_sec, tv.tv_usec, instance_name);
	SHA256_Init(&sha256);
	SHA256_Update(&sha256, inbuf, strlen(inbuf));
	SHA256_Final(hash, &sha256);
	gen_sha256_string(&hash[0], outbuf);
	return (strdup(outbuf));
}

int
dispatch_launch_prison(int sock)
{
	extern struct global_params gcfg;
	struct prison_response resp;
	struct prison_instance *pi;
	char **env, **argv;
	struct prison_launch pl;
	ssize_t cc;
	vec_t *cmd_vec, *env_vec;

	cc = sock_ipc_must_read(sock, &pl, sizeof(pl));
	if (cc == 0) {
		return (0);
	}
	if (!prison_instance_is_unique(pl.p_name)) {
		resp.p_ecode = 1;
		sprintf(resp.p_errbuf, "prison already exists");
		sock_ipc_must_write(sock, &resp, sizeof(resp));
		return (1);
	}
	pi = calloc(1, sizeof(*pi));
	if (pi == NULL) {
		err(1, "calloc failed");
	}
	if (pl.p_entry_point_args[0] != '\0') {
		printf("Passing in command line arguments: %s\n", 
		    pl.p_entry_point_args);
	}
	pi->p_type = PRISON_TYPE_REGULAR;
	strlcpy(pi->p_name, pl.p_name, sizeof(pi->p_name));
	printf("creating process with TERM=%s\n", pl.p_term);
	cmd_vec = vec_init(64);
	env_vec = vec_init(64);
	/*
	 * Setup the environment variables first.
	 */
	char buf[256];
	sprintf(buf, "TERM=%s", pl.p_term);
	vec_append(env_vec, buf);
	vec_finalize(env_vec);
	sprintf(buf, "%s/lib/stage_launch.sh", gcfg.c_data_dir);
	vec_append(cmd_vec, "/bin/sh");
	vec_append(cmd_vec, buf);
	vec_append(cmd_vec, gcfg.c_data_dir);
	vec_append(cmd_vec, pl.p_name);
	pi->p_instance_tag = gen_sha256_instance_id(pl.p_name);
	vec_append(cmd_vec, pi->p_instance_tag);
	printf("Generated instances ID=%s\n", pi->p_instance_tag);
	if (pl.p_entry_point_args[0] != '\0') {
		vec_append(cmd_vec, pl.p_entry_point_args);
	}
	vec_finalize(cmd_vec);
	pi->p_pid = forkpty(&pi->p_ttyfd, pi->p_ttyname, NULL, NULL);
	if (pi->p_pid == 0) {
		argv = vec_return(cmd_vec);
		env = vec_return(env_vec);
		putchar('\n');
		execve(*argv, argv, env);
		err(1, "execve failed");
	}
	TAILQ_INIT(&pi->p_ttybuf.t_head);
	pi->p_ttybuf.t_tot_len = 0;
	pthread_mutex_lock(&prison_mutex);
	TAILQ_INSERT_HEAD(&pr_head, pi, p_glue);
	pthread_mutex_unlock(&prison_mutex);
	resp.p_ecode = 0;
	resp.p_errbuf[0] = '\0';
	sock_ipc_must_write(sock, &resp, sizeof(resp));
	return (1);
}

void *
dispatch_work(void *arg)
{
	struct prison_peer *p;
	uint32_t cmd;
	ssize_t cc;
	int done;

	signal(SIGCHLD, handle_reap_children);
	p = (struct prison_peer *)arg;
	done = 0;
	while (!done) {
		cc = sock_ipc_may_read(p->p_sock, &cmd, sizeof(cmd));
		if (cc == 1) {
			break;
		}
		switch (cmd) {
		case PRISON_IPC_LAUNCH_BUILD:
			cc = dispatch_build_launch(p->p_sock);
			break;
		case PRISON_IPC_SEND_BUILD_CTX:
			cc = dispatch_build_recieve(p->p_sock);
			break;
		case PRISON_IPC_CONSOLE_CONNECT:
			cc = dispatch_connect_console(p->p_sock);
			done = 1;
			break;
		case PRISON_IPC_LAUNCH_PRISON:
			cc = dispatch_launch_prison(p->p_sock);
			if (cc == 0) {
				done = 1;
				continue;
			}
			break;
		default:
			/*
			 * NB: maybe best to send a response
			 */
			warnx("unknown command %u", cmd);
			close(p->p_sock);
			break;
		}
	}
	close(p->p_sock);
	pthread_mutex_lock(&peer_mutex);
	TAILQ_REMOVE(&p_head, p, p_glue);
	pthread_mutex_unlock(&peer_mutex);
	free(p);
	return (NULL);
}

void *
prison_handle_request(void *arg)
{
	struct prison_peer *p;

	p = (struct prison_peer *)arg;
	if (pthread_create(&p->p_thr, NULL, dispatch_work, arg) != 0) {
		err(1, "pthread_create(dispatch_work) failed");
	}
	pthread_mutex_lock(&peer_mutex);
	TAILQ_INSERT_HEAD(&p_head, p, p_glue);
	pthread_mutex_unlock(&peer_mutex);
	return (NULL);
}

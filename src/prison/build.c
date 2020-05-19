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
#include <sys/stat.h>
#include <sys/queue.h>
#include <sys/ioctl.h>
#include <sys/wait.h>
#include <sys/ttycom.h>

#include <stdio.h>
#include <errno.h>
#include <assert.h>
#include <string.h>
#include <getopt.h>
#include <stdlib.h>
#include <err.h>
#include <termios.h>
#include <stdint.h>
#include <fcntl.h>
#include <unistd.h>

#include <libprison.h>

#include "main.h"
#include "parser.h"

struct build_config {
	char			*b_name;
	char			*b_prison_file;
	char			*b_path;
	char			*b_context_path;
	char			*b_tag;
	struct build_manifest	*b_bmp;
	int			 b_verbose;
};

static struct option build_options[] = {
	{ "name",		required_argument, 0, 'n' },
	{ "prison-file-path",	required_argument, 0, 'f' },
	{ "tag",		required_argument, 0, 't' },
	{ "no-exec",		no_argument, 0, 'N' },
	{ "help",		no_argument, 0, 'h' },
	{ "verbose",		no_argument, 0, 'v' },
	{ 0, 0, 0, 0 }
};

struct build_manifest *
build_manifest_load(struct build_config *bcp)
{
	struct build_manifest *bmp;
	char manifest_path[128];
	FILE *f;

	bmp = build_manifest_init();
	if (bmp == NULL) {
		err(1, "failed to get build manifest");
	}
	(void) snprintf(manifest_path, sizeof(manifest_path), "%s/%s",
	    bcp->b_path, bcp->b_prison_file);
	f = fopen(manifest_path, "r");
	if (f == NULL) {
		err(1, "fopen manifest failed");
	}
	yyfile = manifest_path;
	yyin = f;
	set_current_build_manifest(bmp);
	bcp->b_bmp = bmp;
	yyparse();
	fclose(f);
	return (bmp);
}

static void
build_usage(void)
{
	(void) fprintf(stderr,
	    "Usage: prison build [OPTIONS] PATH\n\n"
	    "Options\n"
	    " -h, --help                    Print help\n"
	    " -n, --name=NAME               Name of container image to build\n"
	    " -f, --prison-file-path=PATH   Path to Prisonfile (relative to build path)\n"
	    " -t, --tag=NAME                Tag to use for the image build\n"
	    " -N, --no-exec                 Do everything but submit the build context\n"
	    " -v, --verbose                 Increase verbosity of build\n");
	exit(1);
}

static void
build_init_stage_count(struct build_config *bcp,
    struct prison_build_context *pbc)
{
	struct build_stage *bsp;
	struct build_step *bs;

	pbc->p_nstages = 0;
	pbc->p_nsteps = 0;
	TAILQ_FOREACH(bsp, &bcp->b_bmp->stage_head, stage_glue) {
		pbc->p_nstages++;
		TAILQ_FOREACH(bs, &bsp->step_head, step_glue) {
			pbc->p_nsteps++;
		}
	}
}

static void
build_send_stages(int sock, struct build_config *bcp)
{
	struct build_stage *stage;
	struct build_step *step;

	TAILQ_FOREACH_REVERSE(stage, &bcp->b_bmp->stage_head,
	    tailhead_stage, stage_glue) {
		sock_ipc_must_write(sock, stage, sizeof(*stage));
	}
	TAILQ_FOREACH_REVERSE(stage, &bcp->b_bmp->stage_head,
	    tailhead_stage, stage_glue) {
		TAILQ_FOREACH_REVERSE(step, &stage->step_head,
		    tailhead_step, step_glue) {
			sock_ipc_must_write(sock, step, sizeof(*step));
		}
	}
}

static int
build_send_context(int sock, struct build_config *bcp)
{
	struct prison_build_context pbc;
	struct prison_response resp;
	struct stat sb;
	char *term;
	u_int cmd;
	int fd;

	if (stat(bcp->b_context_path, &sb) == -1) {
		err(1, "stat failed");
	}
	fd = open(bcp->b_context_path, O_RDONLY);
	if (fd == -1) {
		err(1, "error opening build context");
	}
	term = getenv("TERM");
	if (term == NULL) {
		errx(1, "Can not determine TERM type\n");
	}
	printf("Sending build context (%zu) bytes total\n", sb.st_size);
	bzero(&pbc, sizeof(pbc));
	cmd = PRISON_IPC_SEND_BUILD_CTX;
	sock_ipc_must_write(sock, &cmd, sizeof(cmd));
	pbc.p_context_size = sb.st_size;
	pbc.p_verbose = bcp->b_verbose;
	strlcpy(pbc.p_term, term, sizeof(pbc.p_term));
	strlcpy(pbc.p_image_name, bcp->b_name, sizeof(pbc.p_image_name));
	strlcpy(pbc.p_prison_file, bcp->b_prison_file,
	    sizeof(pbc.p_prison_file));
	if (bcp->b_bmp->entry_point) {
		strlcpy(pbc.p_entry_point, bcp->b_bmp->entry_point,
		    sizeof(pbc.p_entry_point));
	}
	if (bcp->b_bmp->entry_point_args) {
		strlcpy(pbc.p_entry_point_args, bcp->b_bmp->entry_point_args,
		    sizeof(pbc.p_entry_point_args));
	}
	strlcpy(pbc.p_tag, bcp->b_tag, sizeof(pbc.p_tag));
	build_init_stage_count(bcp, &pbc);
	sock_ipc_must_write(sock, &pbc, sizeof(pbc));
	build_send_stages(sock, bcp);
	if (sock_ipc_from_to(fd, sock, sb.st_size) == -1) {
		err(1, "sock_ipc_from_to: failed");
	}
	close(fd);
	if (unlink(bcp->b_context_path) == -1) {
		err(1, "failed to cleanup build context");
	}
	sock_ipc_must_read(sock, &resp, sizeof(resp));
	printf("Transfer complete. read status code %d (success) from daemon\n",
	    resp.p_ecode);
	cmd = PRISON_IPC_LAUNCH_BUILD;
	sock_ipc_must_write(sock, &cmd, sizeof(cmd));
	sock_ipc_must_write(sock, &pbc, sizeof(pbc));
	sock_ipc_must_read(sock, &resp, sizeof(resp));
	if (resp.p_ecode != 0) {
		errx(1, "failed to launch build");
	}

	struct prison_console_connect pcc;
	char prison_name[64];

	snprintf(prison_name, sizeof(prison_name), "%s:%s",
	    pbc.p_image_name, pbc.p_tag);
	cmd = PRISON_IPC_CONSOLE_CONNECT;
	if (tcgetattr(STDIN_FILENO, &pcc.p_termios) == -1) {
		err(1, "tcgetattr(STDIN_FILENO) failed");
	}
	if (ioctl(STDIN_FILENO, TIOCGWINSZ, &pcc.p_winsize) == -1) {
		err(1, "ioctl(TIOCGWINSZ): failed");
	}
	strlcpy(pcc.p_name, prison_name, sizeof(pcc.p_name));
	sock_ipc_must_write(sock, &cmd, sizeof(cmd));
	sock_ipc_must_write(sock, &pcc, sizeof(pcc));
	sock_ipc_must_read(sock, &resp, sizeof(resp));
	if (resp.p_ecode != 0) {
		(void) printf("failed to attach console to %s: %s\n",
		    prison_name, resp.p_errbuf);
		return (-1);
	}
	printf("got error code %d\n", resp.p_ecode);
	console_tty_set_raw_mode(STDIN_FILENO);
	console_tty_console_session(sock);
	return (0);
}

static int
build_generate_context(struct build_config *bcp)
{
	char *argv[10], *build_context_path, *template, dst[256];
	int ret, status, pid;

	printf("Constructing build context...");
	fflush(stdout);
	template = strdup("/tmp/prison-bcontext.XXXXXXXXX");
	build_context_path = mktemp(template);
	if (build_context_path == NULL) {
		err(1, "failed to generate random file");
	}
	snprintf(dst, sizeof(dst), "%s.tar.gz", build_context_path);
	build_context_path = mktemp(template);
	pid = fork();
	if (pid == -1) {
		err(1, "fork faild");
	}
	if (pid == 0) {
		argv[0] = "/usr/bin/tar";
		argv[1] = "-C";
		argv[2] = bcp->b_path;
		argv[3] = "-cpf";
		argv[4] = build_context_path;
		argv[5] = ".";
		argv[6] = NULL;
		execve(*argv, argv, NULL);
		err(1, "failed to exec tar for build context");
	}
	while (1) {
		ret = waitpid(pid, &status, 0);
		if (ret == -1 && errno == EINTR) {
			continue;
		} else if (ret == -1) {
			err(1, "waitpid faild");
		}
		break;
		assert(ret == pid);
	}
	if (rename(build_context_path, dst) == -1) {
		err(1, "could not rename build context");
	}
	printf("DONE\n");
	free(template);
	bcp->b_context_path = strdup(dst);
	if (bcp->b_context_path == NULL) {
		err(1, "strdup failed");
	}
	return (status);
}

static void
build_process_stages(struct build_manifest *bmp)
{
	struct build_stage *bsp;

	TAILQ_FOREACH(bsp, &bmp->stage_head, stage_glue) {
		printf("-- FROM %s %p\n", bsp->bs_base_container, bsp);
	}
}

static void
build_set_default_tag(struct build_config *bcp)
{
	char buf[32], *p;

	if (bcp->b_tag != NULL) {
		return;
	}
	snprintf(buf, sizeof(buf), "%ld", time(NULL));
	p = strdup(buf);
	if (p == NULL) {
		err(1, "strdup failed");
	}
	bcp->b_tag = p;
}

int
build_main(int argc, char *argv [], int cltlsock)
{
	struct build_manifest *bmp;
	struct build_config bc;
	time_t before, after;
	int option_index;
	int c, noexec;

	noexec = 0;
	bzero(&bc, sizeof(bc));
	bc.b_prison_file = "Prisonfile";
	reset_getopt_state();
	while (1) {
		option_index = 0;
		c = getopt_long(argc, argv, "Nhf:n:t:v", build_options,
		    &option_index);
		if (c == -1) {
			break;
		}
		switch (c) {
		case 'v':
			bc.b_verbose++;
			break;
		case 'N':
			noexec = 1;
			break;
		case 'h':
			build_usage();
			exit(1);
		case 'n':
			bc.b_name = optarg;
			break;
		case 'f':
			bc.b_prison_file = optarg;
			break;
		case 't':
			bc.b_tag = optarg;
			break;
		default:
			build_usage();
			/* NOT REACHED */
		}
	}
	argc -= optind;
	argv += optind;
	if (bc.b_name == NULL) {
		errx(1, "must specify image name -n");
	}
	bc.b_path = argv[0];
	if (!bc.b_path) {
		fprintf(stderr, "ERROR: no build path specified\n");
		build_usage();
	}
	(void) fprintf(stdout, "building Prison at %s\n", bc.b_path);
	before = time(NULL);
	build_set_default_tag(&bc);
	bmp = build_manifest_load(&bc);
	build_process_stages(bmp);
	if (noexec) {
		return (0);
	}
	build_generate_context(&bc);
	printf("sending context...\n");
	build_send_context(cltlsock, &bc);
	after = time(NULL);
	printf("build occured in %ld seconds\n", after - before);
	return (0);
}

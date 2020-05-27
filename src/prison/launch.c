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
#include <sys/sbuf.h>
#include <sys/ioctl.h>
#include <sys/ttycom.h>

#include <stdio.h>
#include <errno.h>
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

struct launch_config {
	char		*l_name;
	char		*l_terminal;
	vec_t		*l_vec;
	char		*l_volumes;
	char		*l_network;
};

static struct option launch_options[] = {
	{ "name",		required_argument, 0, 'n' },
	{ "terminal",		required_argument, 0, 't' },
	{ "network",		required_argument, 0, 'N' },
	{ "volume",		required_argument, 0, 'V' },
	{ "fdescfs",		no_argument, 0, 'F' },
	{ "procfs",		no_argument, 0, 'p' },
	{ "help",		no_argument, 0, 'h' },
	{ 0, 0, 0, 0 }
};

static void
launch_usage(void)
{
	(void) fprintf(stderr,
	    " -h, --help                  Print help\n"
	    " -n, --name=NAME             Name of container image to launch\n"
	    " -t, --terminal=TERM         Terminal type to use (TERM)\n"
	    " -N, --network=NETWORK       Attach container to specified network\n"
	    " -V, --volume=VOLUMESPEC     Mount volume into the container\n"
	    " -F, --fdescfs               Mount file-descriptor file system\n"
	    " -p, --procfs                Mount process file system\n");
	exit(1);
}

static void
launch_container(int sock, struct launch_config *lcp)
{
	struct prison_launch pl;
	struct prison_response resp;
	char *term, *args;
	uint32_t cmd;

	if (lcp->l_terminal != NULL) {
		printf("setting term\n");
		term = lcp->l_terminal;
	} else {
		term = getenv("TERM");
	}
	bzero(&pl, sizeof(pl));
	cmd = PRISON_IPC_LAUNCH_PRISON;
	if (lcp->l_vec != NULL) {
		args = vec_join(lcp->l_vec, ' ');
		if (args == NULL) {
			err(1, "failed to alloc memory for vec");
		}
		strlcpy(pl.p_entry_point_args, args,
		    sizeof(pl.p_entry_point_args));
		free(args);
		vec_free(lcp->l_vec);
	}
	printf("wriing command\n");
	sock_ipc_must_write(sock, &cmd, sizeof(cmd));
	printf("wrote command %d\n", cmd);
	strlcpy(pl.p_name, lcp->l_name, sizeof(pl.p_name));
	strlcpy(pl.p_term, term, sizeof(pl.p_term));
	strlcpy(pl.p_volumes, lcp->l_volumes, sizeof(pl.p_volumes));
	strlcpy(pl.p_network, lcp->l_network, sizeof(pl.p_network));
	printf("writing args\n");
	sock_ipc_must_write(sock, &pl, sizeof(pl));
	printf("wrote launch structure\n");
	sock_ipc_must_read(sock, &resp, sizeof(resp));
	if (resp.p_ecode == 0) {
		printf("cellblock: container launched: instance: %s\n",
		    resp.p_errbuf);
	}
	printf("resp.p_ecode: %d\n", resp.p_ecode);
}

int
launch_main(int argc, char *argv [], int ctlsock)
{
	struct launch_config lc;
	int option_index, c;
	struct sbuf *sb;


	bzero(&lc, sizeof(lc));
	sb = sbuf_new_auto();
	sbuf_cat(sb, "devfs");
	sbuf_cat(sb, ",");
	lc.l_network = strdup("default");
	reset_getopt_state();
	while (1) {
		option_index = 0;
		c = getopt_long(argc, argv, "N:Fpn:t:V:", launch_options,
		    &option_index);
		if (c == -1) {
			break;
		}
		switch (c) {
		case 'N':
			lc.l_network = optarg;
			break;
		case 'F':
			sbuf_cat(sb, "fdescfs");
			sbuf_cat(sb, ",");
			break;
		case 'p':
			sbuf_cat(sb, "procfs");
			sbuf_cat(sb, ",");
			break;
		case 'V':
			sbuf_cat(sb, optarg);
			sbuf_cat(sb, ",");
			break;
		case 'h':
			launch_usage();
			exit(1);
		case 't':
			lc.l_terminal = optarg;
			break;
		case 'n':
			lc.l_name = optarg;
			break;
		default:
			launch_usage();
			/* NOT REACHED */
		}
	}
	if (lc.l_name == NULL) {
		fprintf(stderr, "must supply container name\n");
		launch_usage();
	}
	sbuf_finish(sb);
	lc.l_volumes = sbuf_data(sb);
	argc -= optind;
	argv += optind;
	/*
	 * Check to see if the user has spcified command line arguments to
	 * along to the entry point for this container.
	 */
	lc.l_vec = NULL;
	if (argc != 0) {
		lc.l_vec = vec_init(argc + 1);
		for (c = 0; c < argc; c++) {
			vec_append(lc.l_vec, argv[c]);
		}
		vec_finalize(lc.l_vec);
	}
	launch_container(ctlsock, &lc);
	return (0);
}

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
};

static struct option launch_options[] = {
	{ "name",		required_argument, 0, 'n' },
	{ "terminal",		required_argument, 0, 't' },
	{ "help",		no_argument, 0, 'h' },
	{ 0, 0, 0, 0 }
};

static void
launch_usage(void)
{
	(void) fprintf(stderr,
	    " -h, --help                  Print help\n"
	    " -n, --name=NAME             Name of container image to launch\n"
	    " -t, --terminal=TERM         Terminal type to use (TERM)\n");
	exit(1);
}

static void
launch_container(int sock, struct launch_config *lcp)
{
	struct prison_launch pl;
	struct prison_response resp;
	uint32_t cmd;
	char *term;

	if (lcp->l_terminal != NULL) {
		printf("setting term\n");
		term = lcp->l_terminal;
	} else {
		term = getenv("TERM");
	}
	cmd = PRISON_IPC_LAUNCH_PRISON;
	sock_ipc_must_write(sock, &cmd, sizeof(cmd));
	strlcpy(pl.p_name, lcp->l_name, sizeof(pl.p_name));
	strlcpy(pl.p_term, term, sizeof(pl.p_term));
	sock_ipc_must_write(sock, &pl, sizeof(pl));
	sock_ipc_must_read(sock, &resp, sizeof(resp));
	printf("got error code %d\n", resp.p_ecode);
}

int
launch_main(int argc, char *argv [], int ctlsock)
{
	struct launch_config lc;
	int option_index;
	int c;

	bzero(&lc, sizeof(lc));
	reset_getopt_state();
	while (1) {
		option_index = 0;
		c = getopt_long(argc, argv, "n:t:", launch_options,
		    &option_index);
		if (c == -1) {
			break;
		}
		switch (c) {
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
	argc -= optind;
	argv += optind;
	launch_container(ctlsock, &lc);
	return (0);
}
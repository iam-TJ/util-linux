/*
 * irqtop.c - utility to display kernel interrupt information.
 *
 * Copyright (C) 2019 zhenwei pi <pizhenwei@bytedance.com>
 * Copyright (C) 2020 Karel Zak <kzak@redhat.com>
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA  02110-1301 USA
 */
#include <ctype.h>
#include <errno.h>
#include <getopt.h>
#include <limits.h>
#include <locale.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/epoll.h>
#include <sys/ioctl.h>
#include <sys/select.h>
#include <sys/signalfd.h>
#include <sys/time.h>
#include <sys/timerfd.h>
#include <sys/types.h>
#include <termios.h>
#include <unistd.h>

#ifdef HAVE_SLCURSES_H
# include <slcurses.h>
#elif defined(HAVE_SLANG_SLCURSES_H)
# include <slang/slcurses.h>
#elif defined(HAVE_NCURSESW_NCURSES_H) && defined(HAVE_WIDECHAR)
# include <ncursesw/ncurses.h>
#elif defined(HAVE_NCURSES_H)
# include <ncurses.h>
#elif defined(HAVE_NCURSES_NCURSES_H)
# include <ncurses/ncurses.h>
#endif

#ifdef HAVE_WIDECHAR
# include <wctype.h>
# include <wchar.h>
#endif

#include <libsmartcols.h>

#include "c.h"
#include "closestream.h"
#include "monotonic.h"
#include "nls.h"
#include "pathnames.h"
#include "strutils.h"
#include "timeutils.h"
#include "ttyutils.h"
#include "xalloc.h"

#include "irq-common.h"

#define MAX_EVENTS	3

struct irqtop_ctl {
	int cols;
	int rows;
	struct itimerspec timer;
	struct irq_stat *prev_stat;
	char *hostname;

	unsigned int request_exit:1;

};

static void parse_input(struct irqtop_ctl *ctl, struct irq_output *out, char c)
{
	switch (c) {
	case 'i':
		out->sort_func = sort_interrupts;
		break;
	case 't':
		out->sort_func = sort_total;
		break;
	case 'd':
		out->sort_func = sort_delta;
		break;
	case 'n':
		out->sort_func = sort_name;
		break;
	case 'q':
	case 'Q':
		ctl->request_exit = 1;
		break;
	}
}

static int update_screen(struct irqtop_ctl *ctl, struct irq_output *out, WINDOW *win)
{
	struct libscols_table *table;
	struct irq_stat *stat;
	time_t now = time(NULL);
	char timestr[64], *data;

	table = get_scols_table(out, ctl->prev_stat, &stat);
	if (!table) {
		ctl->request_exit = 1;
		return 1;
	}

	/* header in interactive mode */
	move(0, 0);
	strtime_iso(&now, ISO_TIMESTAMP, timestr, sizeof(timestr));
	wprintw(win, _("irqtop | total: %ld delta: %ld | %s | %s\n\n"),
			   stat->total_irq, stat->delta_irq, ctl->hostname, timestr);

	scols_print_table_to_string(table, &data);
	wprintw(win, "%s", data);
	free(data);

	/* clean up */
	scols_unref_table(table);
	if (ctl->prev_stat)
		free_irqinfo(ctl->prev_stat);
	ctl->prev_stat = stat;
	return 0;
}

static int event_loop(struct irqtop_ctl *ctl, struct irq_output *out, WINDOW *win)
{
	int efd, sfd, tfd;
	sigset_t sigmask;
	struct signalfd_siginfo siginfo;
	struct epoll_event ev, events[MAX_EVENTS];
	long int nr;
	uint64_t unused;
	int retval = 0;

	efd = epoll_create1(0);

	if ((tfd = timerfd_create(CLOCK_MONOTONIC, 0)) < 0)
		err(EXIT_FAILURE, _("cannot not create timerfd"));
	if (timerfd_settime(tfd, 0, &ctl->timer, NULL) != 0)
		err(EXIT_FAILURE, _("cannot set timerfd"));

	ev.events = EPOLLIN;
	ev.data.fd = tfd;
	if (epoll_ctl(efd, EPOLL_CTL_ADD, tfd, &ev) != 0)
		err(EXIT_FAILURE, _("epoll_ctl failed"));

	if (sigfillset(&sigmask) != 0)
		err(EXIT_FAILURE, _("sigfillset failed"));
	if (sigprocmask(SIG_BLOCK, &sigmask, NULL) != 0)
		err(EXIT_FAILURE, _("sigprocmask failed"));

	sigaddset(&sigmask, SIGWINCH);
	sigaddset(&sigmask, SIGTERM);
	sigaddset(&sigmask, SIGINT);
	sigaddset(&sigmask, SIGQUIT);

	if ((sfd = signalfd(-1, &sigmask, SFD_CLOEXEC)) < 0)
		err(EXIT_FAILURE, _("cannot not create signalfd"));

	ev.events = EPOLLIN;
	ev.data.fd = sfd;
	if (epoll_ctl(efd, EPOLL_CTL_ADD, sfd, &ev) != 0)
		err(EXIT_FAILURE, _("epoll_ctl failed"));

	ev.events = EPOLLIN;
	ev.data.fd = STDIN_FILENO;
	if (epoll_ctl(efd, EPOLL_CTL_ADD, STDIN_FILENO, &ev) != 0)
		err(EXIT_FAILURE, _("epoll_ctl failed"));

	retval |= update_screen(ctl, out, win);
	refresh();

	while (!ctl->request_exit) {
		const ssize_t nr_events = epoll_wait(efd, events, MAX_EVENTS, -1);

		for (nr = 0; nr < nr_events; nr++) {
			if (events[nr].data.fd == tfd) {
				if (read(tfd, &unused, sizeof(unused)) < 0)
					warn(_("read failed"));
			} else if (events[nr].data.fd == sfd) {
				if (read(sfd, &siginfo, sizeof(siginfo)) < 0) {
					warn(_("read failed"));
					continue;
				}
				if (siginfo.ssi_signo == SIGWINCH) {
					get_terminal_dimension(&ctl->cols, &ctl->rows);
					resizeterm(ctl->rows, ctl->cols);
				}
				else {
					ctl->request_exit = 1;
					break;
				}
			} else if (events[nr].data.fd == STDIN_FILENO) {
				char c;

				if (read(STDIN_FILENO, &c, 1) != 1)
					warn(_("read failed"));
				parse_input(ctl, out, c);
			} else
				abort();
			retval |= update_screen(ctl, out, win);
			refresh();
		}
	}
	return retval;
}

static void __attribute__((__noreturn__)) usage(void)
{
	fputs(USAGE_HEADER, stdout);
	printf(_(" %s [options]\n"), program_invocation_short_name);
	fputs(USAGE_SEPARATOR, stdout);

	puts(_("Interactive utility to display kernel interrupt information."));

	fputs(USAGE_OPTIONS, stdout);
	fputs(_(" -d, --delay <secs>   delay updates\n"), stdout);
	fputs(_("     --once           only display interrupts once, then exit\n"), stdout);
	fputs(_(" -J  --json           output json, implies displaying once\n"), stdout);
	fputs(_(" -o  --output <list>  define which output columns to use (see below)\n"), stdout);
	fputs(_(" -s, --sort <char>    specify sort criteria by character (see below)\n"), stdout);
	fputs(USAGE_SEPARATOR, stdout);
	printf(USAGE_HELP_OPTIONS(22));

	fputs(_("\nThe following interactive key commands are valid:\n"), stdout);
	fputs(_("  i      sort by IRQ\n"), stdout);
	fputs(_("  t      sort by TOTAL\n"), stdout);
	fputs(_("  d      sort by DELTA\n"), stdout);
	fputs(_("  n      sort by NAME\n"), stdout);
	fputs(_("  q Q    quit program\n"), stdout);

	fputs(USAGE_COLUMNS, stdout);
	irq_print_columns(stdout);

	printf(USAGE_MAN_TAIL("irqtop(1)"));
	exit(EXIT_SUCCESS);
}

static void parse_args(	struct irqtop_ctl *ctl,
			struct irq_output *out,
			int argc,
			char **argv)
{
	const char *outarg = NULL;
	static const struct option longopts[] = {
		{"delay", required_argument, NULL, 'd'},
		{"sort", required_argument, NULL, 's'},
		{"output", required_argument, NULL, 'o'},
		{"help", no_argument, NULL, 'h'},
		{"version", no_argument, NULL, 'V'},
		{NULL, 0, NULL, 0}
	};
	int o;

	while ((o = getopt_long(argc, argv, "d:o:s:hJV", longopts, NULL)) != -1) {
		switch (o) {
		case 'd':
			{
				struct timeval delay;

				strtotimeval_or_err(optarg, &delay,
						    _("failed to parse delay argument"));
				TIMEVAL_TO_TIMESPEC(&delay, &ctl->timer.it_interval);
				ctl->timer.it_value = ctl->timer.it_interval;
			}
			break;
		case 's':
			out->sort_func = set_sort_func(optarg[0]);
			break;
		case 'o':
			outarg = optarg;
			break;
		case 'V':
			print_version(EXIT_SUCCESS);
		case 'h':
			usage();
		default:
			errtryhelp(EXIT_FAILURE);
		}
	}

	/* default */
	if (!out->ncolumns) {
		out->columns[out->ncolumns++] = COL_IRQ;
		out->columns[out->ncolumns++] = COL_TOTAL;
		out->columns[out->ncolumns++] = COL_DELTA;
		out->columns[out->ncolumns++] = COL_NAME;
	}

	/* add -o [+]<list> to putput */
	if (outarg && string_add_to_idarray(outarg, out->columns,
				ARRAY_SIZE(out->columns),
				&out->ncolumns,
				irq_column_name_to_id) < 0)
		exit(EXIT_FAILURE);
}

int main(int argc, char **argv)
{
	WINDOW *win = NULL;
	int is_tty = 0;
	int retval = EXIT_SUCCESS;
	struct termios saved_tty;
	struct irq_output out = { .ncolumns = 0, .sort_func = DEF_SORT_FUNC };
	struct irqtop_ctl ctl = {
		.timer.it_interval = {3, 0},
		.timer.it_value = {3, 0}
	};

	setlocale(LC_ALL, "");

	parse_args(&ctl, &out, argc, argv);

	is_tty = isatty(STDIN_FILENO);
	if (is_tty && tcgetattr(STDIN_FILENO, &saved_tty) == -1)
		fputs(_("terminal setting retrieval"), stdout);

	win = initscr();
	get_terminal_dimension(&ctl.cols, &ctl.rows);
	resizeterm(ctl.rows, ctl.cols);
	curs_set(0);

	ctl.hostname = xgethostname();
	event_loop(&ctl, &out, win);

	free_irqinfo(ctl.prev_stat);
	free(ctl.hostname);

	if (is_tty)
		tcsetattr(STDIN_FILENO, TCSAFLUSH, &saved_tty);
	delwin(win);
	endwin();

	return retval;
}

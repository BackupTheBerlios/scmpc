/**
 * scmpc.c: The front end of the program.
 *
 * ==================================================================
 * Copyright (c) 2009-2011 Christoph Mende <angelos@unkreativ.org>
 * Based on Jonathan Coome's work on scmpc
 *
 * This file is part of scmpc.
 *
 * scmpc is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * scmpc is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with scmpc; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA  02110-1301  USA
 * ==================================================================
 */


#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <stdlib.h>
#include <unistd.h>
#include <sys/stat.h>

#include <mpd/client.h>

#include "misc.h"
#include "audioscrobbler.h"
#include "preferences.h"
#include "queue.h"
#include "scmpc.h"
#include "mpd.h"

/* Static function prototypes */
static gint scmpc_is_running(void);
static gint scmpc_pid_create(void);
static gint scmpc_pid_remove(void);

static void sighandler(gint sig);
static void signal_parse(void);
static void daemonise(void);
static void cleanup(void);

static gboolean current_song_eligible_for_submission(void);

static int signal_pipe[2] = { -1, -1 };

int main(int argc, char *argv[])
{
	pid_t pid;
	struct pollfd fds[2];
	struct sigaction sa;
	gboolean mpd_connected = FALSE;
	time_t last_queue_save = 0, mpd_last_fail = 0;

	g_log_set_always_fatal(G_LOG_LEVEL_CRITICAL);

	if (init_preferences(argc, argv) < 0)
		g_critical("Config file parsing failed");

	/* Open the log file before forking, so that if there is an error, the
	 * user will get some idea what is going on */
	open_log(prefs.log_file);

	g_log_set_default_handler(scmpc_log, NULL);

	/* Check if scmpc is already running */
	if ((pid = scmpc_is_running()) > 0) {
		clear_preferences();
		g_critical("Daemon is already running with PID: %ld", (long)pid);
	}

	/* Daemonise if wanted */
	if (prefs.fork)
		daemonise();

	/* Signal handler */
	if (pipe(signal_pipe) < 0)
		g_critical("Opening signal pipe failed, signals will not be "
				"caught: %s", g_strerror(errno));
	else if (fcntl(signal_pipe[1], F_SETFL, fcntl(signal_pipe[1], F_GETFL) | O_NONBLOCK) < 0)
		g_critical("Setting flags on signal pipe failed, signals will "
				"not be caught: %s", g_strerror(errno));
	sa.sa_handler = sighandler;
	sigfillset(&sa.sa_mask);
	sa.sa_flags = SA_RESTART;
	sigaction(SIGINT, &sa, NULL);
	sigaction(SIGTERM, &sa, NULL);
	sigaction(SIGQUIT, &sa, NULL);

	if (as_connection_init() < 0) {
		cleanup();
		exit(EXIT_FAILURE);
	}
	as_authenticate();

	mpd_connected = mpd_connect();
	if (!mpd_connected) {
		mpd_last_fail = time(NULL);
		mpd_connection_free(mpd.conn);
		mpd.conn = NULL;
	}

	queue_load();
	last_queue_save = time(NULL);
	mpd.song_pos = g_timer_new();

	fds[0].events = POLLIN;
	fds[1].events = POLLIN;

	for (;;)
	{
		/* submit queue if not playing */
		if (mpd_connected && (mpd_status_get_state(mpd.status) != MPD_STATE_PLAY || (queue.last && queue.last->finished_playing == TRUE && mpd.song_submitted == TRUE)))
			as_check_submit();

		if (mpd_connected)
			fds[0].fd = mpd_connection_get_fd(mpd.conn);
		else
			fds[0].fd = -1;
		fds[1].fd = signal_pipe[0];
		poll(fds, 2, prefs.mpd_interval);

		/* Check for new events on MPD socket */
		if (fds[0].revents & POLLIN) {
			mpd_connected = mpd_parse();
		}

		/* Check for new events on signal pipe */
		if (fds[1].revents & POLLIN) {
			signal_parse();
		}

		/* Check if MPD socket disconnected */
		if (fds[0].revents & POLLHUP) {
			mpd_connected = FALSE;
			mpd_connection_free(mpd.conn);
			mpd.conn = NULL;
			g_message("Disconnected from MPD, reconnecting");
		}

		/* Check if song is eligible for submission */
		if (current_song_eligible_for_submission())
			queue_add_current_song();

		/* save queue */
		if (difftime(time(NULL), last_queue_save) >= prefs.cache_interval * 60) {
			queue_save();
			last_queue_save = time(NULL);
		}

		/* reconnect to MPD */
		if (!mpd_connected && difftime(time(NULL), mpd_last_fail) >= 1800) {
			mpd_connected = mpd_connect();
			if (!mpd_connected) {
				mpd_connection_free(mpd.conn);
				mpd.conn = NULL;
				mpd_last_fail = time(NULL);
			}
		}
	}
}

static gint scmpc_is_running(void)
{
	FILE *pid_file = fopen(prefs.pid_file, "r");
	pid_t pid;

	if (!pid_file && errno == ENOENT)
		return 0;

	if (!pid_file) {
		/* Unable to open PID file, returning error */
		g_warning("Cannot open pid file (%s) for reading: %s",
				prefs.pid_file, g_strerror(errno));
		return -1;
	}

	if (fscanf(pid_file, "%d", &pid) < 1) {
		/* Read nothing from pid_file */
		fclose(pid_file);
		if (unlink(prefs.pid_file) < 0) {
			/* Unable to remove invalid PID file, returning error */
			g_warning("Invalid pid file %s cannot be removed, "
				"please remove this file or change pid_file in "
				"your configuration.", prefs.pid_file);
			return -1;
		} else {
			/* Invalid PID file removed, start new instance */
			g_message("Invalid pid file %s removed.",
					prefs.pid_file);
			return 0;
		}
	}

	fclose(pid_file);

	if (!kill(pid, 0)) {
		/* scmpc already running */
		return pid;
	} else if (errno == ESRCH) {
		/* no such process */
		if (unlink(prefs.pid_file) < 0) {
			/* Unable to remove invalid pid file, returning error */
			fprintf(stderr, "Old pid file %s cannot be removed, "
				" please remove this file or change pid_file in"
				" your configuration.", prefs.pid_file);
			return -1;
		} else {
			/* Old pid file removed, starting new instance */
			puts("Old pid file removed.");
			return 0;
		}
	}

	return 0;
}

static gint scmpc_pid_create(void)
{
	FILE *pid_file = fopen(prefs.pid_file, "w");
	if (!pid_file) {
		g_warning("Cannot open pid file (%s) for writing: %s",
				prefs.pid_file, g_strerror(errno));
		return -1;
	}

	fprintf(pid_file, "%u\n", getpid());
	fclose(pid_file);
	return 0;
}

static gint scmpc_pid_remove(void)
{
	if (unlink(prefs.pid_file) < 0) {
		g_warning("Could not remove pid file: %s", g_strerror(errno));
		return 1;
	}
	return 0;

}

static void sighandler(gint sig)
{
	if (write(signal_pipe[1], &sig, 1) < 0) {
		g_message("Reading from signal pipe failed, closing pipe.");
		close(signal_pipe[0]);
		close(signal_pipe[1]);
		signal_pipe[0] = -1;
		signal_pipe[1] = -1;
	}
}

static void signal_parse(void)
{
	gchar sig;
	if (read(signal_pipe[0], &sig, 1) < 0) {
		g_message("Reading from signal pipe failed, "
				"closing pipe.");
		close(signal_pipe[0]);
		close(signal_pipe[1]);
		signal_pipe[0] = -1;
		signal_pipe[1] = -1;
	}
	g_message("Caught signal %hhd, exiting.", sig);
	cleanup();
	exit(EXIT_SUCCESS);
}

static void daemonise(void)
{
	pid_t pid;

	if ((pid = fork()) < 0) {
		/* Something went wrong... */
		clear_preferences();
		g_critical("Could not fork process.");
	} else if (pid) { /* The parent */
		exit(EXIT_SUCCESS);
	} else { /* The child */
		/* Force sane umask */
		umask(022);

		/* Create the PID file */
		if (scmpc_pid_create() < 0) {
			clear_preferences();
			g_critical("Failed to create PID file");
		}
	}
}

static void cleanup(void)
{
	if (current_song_eligible_for_submission())
		queue_add_current_song();
	if (prefs.fork)
		scmpc_pid_remove();
	if (signal_pipe[0] > 0) {
		close(signal_pipe[0]);
		close(signal_pipe[1]);
	}
	queue_save();
	if (mpd.song_pos)
		g_timer_destroy(mpd.song_pos);
	clear_preferences();
	as_cleanup();
	if (mpd.conn != NULL)
		mpd_connection_free(mpd.conn);
}

void kill_scmpc(void)
{
	FILE *pid_file = fopen(prefs.pid_file, "r");
	pid_t pid;

	if (!pid_file)
		g_critical("Unable to open PID file: %s", g_strerror(errno));

	if (fscanf(pid_file, "%d", &pid) < 1)
		g_critical("Invalid PID file");

	if (kill(pid, SIGTERM) < 0)
		g_critical("Cannot kill running scmpc");

	exit(EXIT_SUCCESS);
}

static gboolean current_song_eligible_for_submission(void)
{
	if (!mpd.song)
		return FALSE;

	return (!mpd.song_submitted &&
			(g_timer_elapsed(mpd.song_pos, NULL) >= 240 ||
			 g_timer_elapsed(mpd.song_pos, NULL) >=
				mpd_song_get_duration(mpd.song) / 2));
}

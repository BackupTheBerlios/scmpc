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
static gboolean scmpc_check(gpointer data);
static gint scmpc_is_running(void);
static gint scmpc_pid_create(void);
static gint scmpc_pid_remove(void);
static void scmpc_cleanup(void);

static void sighandler(gint sig);
static gboolean signal_parse(GIOChannel *source, GIOCondition condition,
		gpointer data);
static gboolean open_signal_pipe(void);
static void close_signal_pipe(void);
static int signal_pipe[2] = { -1, -1 };

static void daemonise(void);
static gboolean current_song_eligible_for_submission(void);

static guint signal_source, cache_save_source, check_source, reconnect_source;
static GMainLoop *loop;

int main(int argc, char *argv[])
{
	pid_t pid;
	struct sigaction sa;

	if (init_preferences(argc, argv) < 0)
		g_error("Config file parsing failed");

	/* Open the log file before forking, so that if there is an error, the
	 * user will get some idea what is going on */
	open_log(prefs.log_file);

	g_log_set_default_handler(scmpc_log, NULL);

	/* Check if scmpc is already running */
	if ((pid = scmpc_is_running()) > 0) {
		clear_preferences();
		g_error("Daemon is already running with PID: %ld", (long)pid);
	}

	/* Daemonise if wanted */
	if (prefs.fork)
		daemonise();

	/* Signal handler */
	open_signal_pipe();
	sa.sa_handler = sighandler;
	sigfillset(&sa.sa_mask);
	sa.sa_flags = SA_RESTART;
	sigaction(SIGINT, &sa, NULL);
	sigaction(SIGTERM, &sa, NULL);
	sigaction(SIGQUIT, &sa, NULL);

	if (as_connection_init() < 0) {
		scmpc_cleanup();
		exit(EXIT_FAILURE);
	}
	as_authenticate();

	queue_load();

	// submit the loaded queue
	as_check_submit();

	mpd.connected = mpd_connect();
	if (!mpd.connected) {
		mpd_connection_free(mpd.conn);
		mpd.conn = NULL;
	}

	mpd.song_pos = g_timer_new();

	// set up main loop events
	loop = g_main_loop_new(NULL, FALSE);

	// save queue
	cache_save_source = g_timeout_add_seconds(prefs.cache_interval * 60,
			queue_save, NULL);

	// reconnect if disconnected
	reconnect_source = g_timeout_add_seconds(300, mpd_reconnect, NULL);

	// check if song is eligible for submission
	check_source = g_timeout_add_seconds(prefs.mpd_interval, scmpc_check,
			NULL);

	g_main_loop_run(loop);

	scmpc_cleanup();
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
		g_message("Writing to signal pipe failed, re-opening pipe.");
		close_signal_pipe();
		open_signal_pipe();
	}
}

static gboolean open_signal_pipe(void)
{
	GIOChannel *channel;

	if (pipe(signal_pipe) < 0) {
		g_critical("Opening signal pipe failed, signals will not be "
				"caught: %s", g_strerror(errno));
		return FALSE;
	}

	if (fcntl(signal_pipe[1], F_SETFL, fcntl(signal_pipe[1], F_GETFL)
				| O_NONBLOCK) < 0) {
		g_critical("Setting flags on signal pipe failed, signals will "
				"not be caught: %s", g_strerror(errno));
		return FALSE;
	}

	channel = g_io_channel_unix_new(signal_pipe[0]);
	signal_source = g_io_add_watch(channel, G_IO_IN, signal_parse,
			NULL);
	g_io_channel_unref(channel);
	return TRUE;
}

static void close_signal_pipe(void)
{
	if (signal_pipe[0] > 0)
		close(signal_pipe[0]);
	if (signal_pipe[1] > 0)
		close(signal_pipe[1]);
	signal_pipe[0] = -1;
	signal_pipe[1] = -1;
}

static gboolean signal_parse(GIOChannel *source,
		G_GNUC_UNUSED GIOCondition condition,
		G_GNUC_UNUSED gpointer data)
{
	gint fd = g_io_channel_unix_get_fd(source);
	gchar sig;
	if (read(fd, &sig, 1) < 0) {
		g_message("Reading from signal pipe failed, re-opening pipe.");
		close_signal_pipe();
		open_signal_pipe();
		return TRUE;
	} else {
		g_message("Caught signal %hhd, exiting.", sig);
		scmpc_shutdown();
		return TRUE;
	}
}

static void daemonise(void)
{
	pid_t pid;

	if ((pid = fork()) < 0) {
		/* Something went wrong... */
		clear_preferences();
		g_error("Could not fork process.");
	} else if (pid) { /* The parent */
		exit(EXIT_SUCCESS);
	} else { /* The child */
		/* Force sane umask */
		umask(022);

		/* Create the PID file */
		if (scmpc_pid_create() < 0) {
			clear_preferences();
			g_error("Failed to create PID file");
		}
	}
}

void scmpc_shutdown(void)
{
	if (g_main_loop_is_running(loop))
		g_main_loop_quit(loop);
}

static void scmpc_cleanup(void)
{
	g_source_remove(signal_source);
	g_source_remove(mpd.source);
	g_source_remove(cache_save_source);
	g_source_remove(check_source);
	g_source_remove(reconnect_source);

	if (current_song_eligible_for_submission())
		queue_add_current_song();
	if (prefs.fork)
		scmpc_pid_remove();
	close_signal_pipe();
	queue_save(NULL);
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

static gboolean scmpc_check(G_GNUC_UNUSED gpointer data)
{
	if (current_song_eligible_for_submission())
		queue_add_current_song();
	return TRUE;
}

/**
 * audioscrobbler.c: Audioscrobbler backend.
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


#include <stdlib.h>
#include <string.h>

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include <mpd/client.h>

#include "misc.h"
#include "preferences.h"
#include "audioscrobbler.h"
#include "queue.h"
#include "scmpc.h"
#include "mpd.h"

static gchar curl_error_buffer[CURL_ERROR_SIZE];
static void as_parse_error(char *response);
static gint as_submit(void);

#define API_URL "http://ws.audioscrobbler.com/2.0/"
#define API_KEY "3ec5638071c41a864bf0c8d451566476"
#define API_SECRET "365e18391ccdee3bf820cb3d2ba466f6"

gint as_connection_init(void)
{
	as_conn.handle = curl_easy_init();
	if (!as_conn.handle)
		return -1;
	as_conn.submit_url = as_conn.session_id = NULL;
	as_conn.last_auth = 0;
	as_conn.status = DISCONNECTED;
	as_conn.headers = curl_slist_append(as_conn.headers,
			"User-Agent: scmpc/" PACKAGE_VERSION);

	curl_easy_setopt(as_conn.handle, CURLOPT_HTTPHEADER, as_conn.headers);
	curl_easy_setopt(as_conn.handle, CURLOPT_WRITEFUNCTION, &buffer_write);
	curl_easy_setopt(as_conn.handle, CURLOPT_ERRORBUFFER,curl_error_buffer);
	curl_easy_setopt(as_conn.handle, CURLOPT_NOSIGNAL, 1L);
	curl_easy_setopt(as_conn.handle, CURLOPT_CONNECTTIMEOUT, 5L);
	curl_easy_setopt(as_conn.handle, CURLOPT_TIMEOUT, 5L);
	return 0;
}

void as_cleanup(void)
{
	curl_slist_free_all(as_conn.headers);
	curl_easy_cleanup(as_conn.handle);
	as_conn.headers = as_conn.handle = NULL;
	g_free(as_conn.session_id);
	g_free(as_conn.submit_url);
}

void as_authenticate(void)
{
	gchar *auth_token, *api_sig, *auth_url, *tmp;
	gint ret;

	if (as_conn.status == BADAUTH) {
		g_message("Refusing authentication, please check your "
			"Audioscrobbler credentials and restart %s",
			PACKAGE_NAME);
		return;
	}

	if (!strlen(prefs.as_username) || (!strlen(prefs.as_password) &&
		!strlen(prefs.as_password_hash))) {
		g_message("No username or password specified. "
				"Not connecting to Audioscrobbler.");
		as_conn.status = BADAUTH;
		return;
	}

	if (difftime(time(NULL), as_conn.last_auth) < 1800) {
		g_debug("Requested authentication, but last try "
				"was less than 30 minutes ago.");
		return;
	}

	// compute auth_token
	if (strlen(prefs.as_password_hash) > 0) {
		tmp = g_strdup_printf("%s%s", prefs.as_username,
				prefs.as_password_hash);
	} else {
		auth_token = g_compute_checksum_for_string(G_CHECKSUM_MD5,
				prefs.as_password, -1);
		tmp = g_strdup_printf("%s%s", prefs.as_username, auth_token);
		g_free(auth_token);
	}
	auth_token = g_compute_checksum_for_string(G_CHECKSUM_MD5, tmp, -1);
	g_free(tmp);

	// compute api_sig
	tmp = g_strdup_printf("api_key" API_KEY "authToken%smethod"
			"auth.getMobileSessionusername%s" API_SECRET,
			auth_token, prefs.as_username);
	api_sig = g_compute_checksum_for_string(G_CHECKSUM_MD5, tmp, -1);
	g_free(tmp);

	auth_url = g_strdup_printf(API_URL "?method=auth.getMobileSession"
			"&username=%s&authToken=%s&api_key=" API_KEY "&api_sig="
			"%s", prefs.as_username, auth_token, api_sig);
	g_free(auth_token);
	g_free(api_sig);

	g_debug("auth_url = %s", auth_url);

	curl_easy_setopt(as_conn.handle, CURLOPT_WRITEDATA, buffer);
	curl_easy_setopt(as_conn.handle, CURLOPT_HTTPGET, 1);
	curl_easy_setopt(as_conn.handle, CURLOPT_URL, auth_url);

	ret = curl_easy_perform(as_conn.handle);
	g_free(auth_url);

	if (ret) {
		g_warning("Could not connect to the Audioscrobbler: %s",
			curl_easy_strerror(ret));
		g_free(buffer);
		buffer = NULL;
		return;
	}

	as_conn.last_auth = time(NULL);
	if (!buffer) {
		g_debug("Could not parse Audioscrobbler response.");
		g_free(buffer);
		buffer = NULL;
		return;
	}

	if (strstr(buffer, "<lfm status=\"ok\">")) {
		char *tmp = strstr(buffer, "<key>") + 5;
		as_conn.session_id = g_strndup(tmp, strcspn(tmp, "<"));
		g_message("Connected to Audioscrobbler.");
		as_conn.status = CONNECTED;
	} else if (strstr(buffer, "<lfm status=\"failed\">")) {
		as_parse_error(buffer);
	} else {
		g_debug("Could not parse Audioscrobbler response");
	}
	g_free(buffer);
	buffer = NULL;
}

void as_now_playing(void)
{
	gchar *querystring, *tmp, *sig, *artist, *album, *title, *track;
	gint ret, length;

	if (as_conn.status != CONNECTED) {
		g_message("Not sending Now Playing notification:"
				" not connected");
		return;
	}

	// TODO: implement this without casts
	artist = (gchar*) mpd_song_get_tag(mpd.song, MPD_TAG_ARTIST, 0);
	title = (gchar*) mpd_song_get_tag(mpd.song, MPD_TAG_TITLE, 0);
	album = (gchar*) mpd_song_get_tag(mpd.song, MPD_TAG_ALBUM, 0);
	track = (gchar*) mpd_song_get_tag(mpd.song, MPD_TAG_TRACK, 0);
	length = mpd_song_get_duration(mpd.song);

	tmp = g_strdup_printf("album%sapi_key" API_KEY "artist%sduration%d"
			"methodtrack.updateNowPlayingsk%strack%strackNumber%s"
			API_SECRET, album, artist, length, as_conn.session_id,
			title, track);
	sig = g_compute_checksum_for_string(G_CHECKSUM_MD5, tmp, -1);
	g_free(tmp);

	artist = curl_easy_escape(as_conn.handle, artist, 0);
	title = curl_easy_escape(as_conn.handle, title, 0);
	album = curl_easy_escape(as_conn.handle, album, 0);
	track = curl_easy_escape(as_conn.handle, track, 0);

	querystring = g_strdup_printf("album=%s&api_key=" API_KEY "&artist=%s"
			"&duration=%d&method=track.updateNowPlaying&sk=%s"
			"&track=%s&trackNumber=%s&api_sig=%s",
			album, artist, length, as_conn.session_id, title,
			track, sig);

	curl_free(album);
	curl_free(artist);
	curl_free(title);
	curl_free(track);
	g_free(sig);

	g_debug("querystring = %s", querystring);

	curl_easy_setopt(as_conn.handle, CURLOPT_WRITEDATA, buffer);
	curl_easy_setopt(as_conn.handle, CURLOPT_POSTFIELDS, querystring);
	curl_easy_setopt(as_conn.handle, CURLOPT_URL, API_URL);

	ret = curl_easy_perform(as_conn.handle);
	g_free(querystring);
	if (ret) {
		g_warning("Failed to connect to Audioscrobbler: %s",
			curl_easy_strerror(ret));
		g_free(buffer);
		buffer = NULL;
		return;
	}

	if (strstr(buffer, "<lfm status=\"ok\">")) {
		g_message("Sent Now Playing notification.");
	} else if (strstr(buffer, "<lfm status=\"failed\">")) {
		as_parse_error(buffer);
	} else {
		g_debug("Unknown response from Audioscrobbler while "
			"sending Now Playing notification.");
	}

	g_free(buffer);
	buffer = NULL;
}

static gint build_querystring(gchar **qs, queue_node **last_song)
{
	gchar *sig, *tmp;
	GString *nqs;
	GString *albums, *artists, *lengths, *timestamps, *titles;
	GString *tracks;
	gshort num = 0;
	queue_node *song = queue.first;

	nqs = g_string_new("api_key=" API_KEY "&method=track.scrobble&sk=");
	g_string_append(nqs, as_conn.session_id);

	albums = g_string_new("");
	artists = g_string_new("");
	lengths = g_string_new("");
	timestamps = g_string_new("");
	titles = g_string_new("");
	tracks = g_string_new("");

	while (song && num < 10) {
		gchar *album, *artist, *title, *track;

		if (!song->finished_playing) {
			song = song->next;
			continue;
		}

		g_string_append_printf(albums, "album[%d]%s", num, song->album);
		g_string_append_printf(artists, "artist[%d]%s", num,
				song->artist);
		g_string_append_printf(lengths, "duration[%d]%d", num,
				song->length);
		g_string_append_printf(timestamps, "timestamp[%d]%ld", num,
				song->date);
		g_string_append_printf(titles, "track[%d]%s", num, song->title);
		g_string_append_printf(tracks, "trackNumber[%d]%s", num,
				song->track);

		album = curl_easy_escape(as_conn.handle, song->album, 0);
		artist = curl_easy_escape(as_conn.handle, song->artist, 0);
		title = curl_easy_escape(as_conn.handle, song->title, 0);
		track = curl_easy_escape(as_conn.handle, song->track, 0);

		g_string_append_printf(nqs, "&album[%d]=%s&artist[%d]=%s"
				"&duration[%d]=%d&timestamp[%d]=%ld"
				"&track[%d]=%s&trackNumber[%d]=%s",
				num, album, num, artist, num, song->length, num,
				song->date, num, title, num, track);

		curl_free(album); curl_free(artist); curl_free(title);
		curl_free(track);

		num++;
		song = song->next;
	}

	tmp = g_strdup_printf("%sapi_key" API_KEY "%s%smethodtrack.scrobble"
			"sk%s%s%s%s" API_SECRET, albums->str, artists->str,
			lengths->str, as_conn.session_id, timestamps->str,
			tracks->str, titles->str);
	sig = g_compute_checksum_for_string(G_CHECKSUM_MD5, tmp, -1);
	g_free(tmp);
	g_string_free(albums, TRUE);
	g_string_free(artists, TRUE);
	g_string_free(lengths, TRUE);
	g_string_free(timestamps, TRUE);
	g_string_free(titles, TRUE);
	g_string_free(tracks, TRUE);

	g_string_append_printf(nqs, "&api_sig=%s", sig);
	g_free(sig);

	*qs = g_string_free(nqs, FALSE);
	*last_song = song;
	return num;
}

static gint as_submit(void)
{
	gchar *querystring;
	queue_node *last_added;
	gint ret, num_songs;

	if (!queue.first)
		return -1;

	num_songs = build_querystring(&querystring, &last_added);
	if (num_songs <= 0) {
		g_free(querystring);
		return -1;
	}

	g_debug("querystring = %s", querystring);

	curl_easy_setopt(as_conn.handle, CURLOPT_WRITEDATA, buffer);
	curl_easy_setopt(as_conn.handle, CURLOPT_POSTFIELDS, querystring);
	curl_easy_setopt(as_conn.handle, CURLOPT_URL, API_URL);

	ret = curl_easy_perform(as_conn.handle);
	g_free(querystring);
	if (ret != 0) {
		g_message("Failed to connect to Audioscrobbler: %s",
			curl_easy_strerror(ret));
		return 1;
	}

	if (strstr(buffer, "<lfm status=\"ok\">")) {
		g_message("%d song%s submitted.", num_songs,
				(num_songs > 1 ? "s" : ""));
		queue_remove_songs(queue.first, last_added);
		queue.first = last_added;
	} else if (strstr(buffer, "<lfm status=\"failed\">")) {
		as_parse_error(buffer);
	} else {
		g_message("Could not parse Audioscrobbler submit"
				" response.");
	}

	g_free(buffer);
	buffer = NULL;

	return 0;
}

static void as_parse_error(char *response)
{
	char *tmp, *message;
	int code;

	tmp = strstr(response, "<error code=\"") + 13;
	code = g_ascii_strtoll(tmp, NULL, 0);

	switch(code) {
		case 4:
			as_conn.status = BADAUTH;
			break;
		case 9:
			as_authenticate();
			break;
		default:
			break;
	}

	tmp = strstr(tmp, "\">") + 2;
	message = g_strndup(tmp, strcspn(tmp, "<"));
	g_warning("%s", message);
	g_free(message);
}

void as_check_submit(void)
{
	if (queue.length > 0 && as_conn.status == CONNECTED &&
			difftime(time(NULL), as_conn.last_fail) >= 600) {
		if (as_submit() == 1)
		as_conn.last_fail = time(NULL);
	}
}

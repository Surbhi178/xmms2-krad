/** @file mp4.c
 *  Decoder plugin for MP4 audio format
 *
 *  Copyright (C) 2005-2006 XMMS2 Team
 *
 *  This program is free software; you can redistribute it and/or
 *  modify it under the terms of the GNU General Public License
 *  as published by the Free Software Foundation; either version 2
 *  of the License, or (at your option) any later version.
 *
 *  This program is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *  GNU General Public License for more details.
 */

#include "xmms/xmms_defs.h"
#include "xmms/xmms_xformplugin.h"
#include "xmms/xmms_bindata.h"
#include "xmms/xmms_sample.h"
#include "xmms/xmms_log.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <faad.h>
#include <glib.h>

#include "mp4ff/mp4ff.h"

#define FAAD_BUFFER_SIZE 4096

typedef struct {
	faacDecHandle decoder;
	gint filetype;

	mp4ff_t *mp4ff;
	mp4ff_callback_t *mp4ff_cb;
	gint track;
	glong sampleid;
	glong numsamples;
	gint toskip;

	guchar buffer[FAAD_BUFFER_SIZE];
	guint buffer_length;
	guint buffer_size;

	guint channels;
	guint bitrate;
	guint samplerate;
	xmms_sample_format_t sampleformat;

	GString *outbuf;
} xmms_mp4_data_t;

static gboolean xmms_mp4_plugin_setup (xmms_xform_plugin_t *xform_plugin);
static gboolean xmms_mp4_init (xmms_xform_t *xform);
static void xmms_mp4_destroy (xmms_xform_t *xform);
static gint xmms_mp4_read (xmms_xform_t *xform, xmms_sample_t *buf, gint len, xmms_error_t *err);
static gint64 xmms_mp4_seek (xmms_xform_t *xform, gint64 samples, xmms_xform_seek_mode_t whence, xmms_error_t *err);
static void xmms_mp4_get_mediainfo (xmms_xform_t *xform);

uint32_t xmms_mp4_read_callback (void *user_data, void *buffer, uint32_t length);
uint32_t xmms_mp4_seek_callback (void *user_data, uint64_t position);
int xmms_mp4_get_aac_track (mp4ff_t *infile);

/*
 * Plugin header
 */

XMMS_XFORM_PLUGIN ("mp4",
                   "MP4 Demuxer", XMMS_VERSION,
                   "MPEG-4 Part 14 file format demuxer",
                   xmms_mp4_plugin_setup);

static gboolean
xmms_mp4_plugin_setup (xmms_xform_plugin_t *xform_plugin)
{
	xmms_xform_methods_t methods;

	XMMS_XFORM_METHODS_INIT (methods);
	methods.init = xmms_mp4_init;
	methods.destroy = xmms_mp4_destroy;
	methods.read = xmms_mp4_read;
	methods.seek = xmms_mp4_seek;

	xmms_xform_plugin_methods_set (xform_plugin, &methods);

	xmms_xform_plugin_indata_add (xform_plugin,
	                              XMMS_STREAM_TYPE_MIMETYPE,
	                              "video/mp4",
	                              NULL);

	xmms_xform_plugin_indata_add (xform_plugin,
	                              XMMS_STREAM_TYPE_MIMETYPE,
	                              "audio/mp4",
	                              NULL);

	xmms_magic_add ("mpeg-4 header", "video/mp4",
	                "4 string ftyp",
	                ">8 string isom",
	                ">8 string mp41",
	                ">8 string mp42",
	                NULL);

	xmms_magic_add ("iTunes mpeg-4 header", "audio/mp4",
	                "4 string ftyp", ">8 string M4A ", NULL);

	return TRUE;
}

static void
xmms_mp4_destroy (xmms_xform_t *xform)
{
	xmms_mp4_data_t *data;

	g_return_if_fail (xform);

	data = xmms_xform_private_data_get (xform);
	g_return_if_fail (data);

	faacDecClose (data->decoder);
	if (data->mp4ff) {
		mp4ff_close (data->mp4ff);
	}
	g_free (data->mp4ff_cb);

	g_string_free (data->outbuf, TRUE);
	g_free (data);
}

static gboolean
xmms_mp4_init (xmms_xform_t *xform)
{
	xmms_mp4_data_t *data;
	xmms_error_t error;

	faacDecConfigurationPtr config;
	gint bytes_read;
	gulong samplerate;
	guchar channels;

	guchar *tmpbuf;
	guint tmpbuflen;

	g_return_val_if_fail (xform, FALSE);

	data = g_new0 (xmms_mp4_data_t, 1);
	data->outbuf = g_string_new (NULL);
	data->buffer_size = FAAD_BUFFER_SIZE;

	xmms_xform_private_data_set (xform, data);

	data->sampleid = 0;
	data->numsamples = 0;
	data->decoder = faacDecOpen ();
	config = faacDecGetCurrentConfiguration (data->decoder);
	config->defObjectType = LC;
	config->defSampleRate = 44100;
	config->outputFormat = FAAD_FMT_16BIT;
	config->downMatrix = 0;
	config->dontUpSampleImplicitSBR = 0;
	faacDecSetConfiguration (data->decoder, config);

	switch (config->outputFormat) {
	case FAAD_FMT_16BIT:
		data->sampleformat = XMMS_SAMPLE_FORMAT_S16;
		break;
	case FAAD_FMT_24BIT:
		/* we don't have 24-bit format to use in xmms2 */
		data->sampleformat = XMMS_SAMPLE_FORMAT_S32;
		break;
	case FAAD_FMT_32BIT:
		data->sampleformat = XMMS_SAMPLE_FORMAT_S32;
		break;
	case FAAD_FMT_FLOAT:
		data->sampleformat = XMMS_SAMPLE_FORMAT_FLOAT;
		break;
	case FAAD_FMT_DOUBLE:
		data->sampleformat = XMMS_SAMPLE_FORMAT_DOUBLE;
		break;
	}

	bytes_read = xmms_xform_read (xform,
	                              (gchar *) data->buffer + data->buffer_length,
	                              data->buffer_size - data->buffer_length,
	                              &error);

	data->buffer_length += bytes_read;

	if (bytes_read < 8) {
		XMMS_DBG ("Not enough bytes to check the MP4 header");
		goto err;
	}

	/*
	 * MP4 not supported (yet) on non-seekable transport
	 * this needs little tweaking in mp4ff at least
	 */
	if (xmms_xform_seek (xform, 0, XMMS_XFORM_SEEK_CUR, &error) < 0) {
		XMMS_DBG ("Non-seekable transport on MP4 not yet supported");
		goto err;
	}

	data->mp4ff_cb = g_new0 (mp4ff_callback_t, 1);
	data->mp4ff_cb->read = xmms_mp4_read_callback;
	data->mp4ff_cb->seek = xmms_mp4_seek_callback;
	data->mp4ff_cb->user_data = xform;

	data->mp4ff = mp4ff_open_read (data->mp4ff_cb);
	if (!data->mp4ff) {
		XMMS_DBG ("Error opening mp4 demuxer\n");
		goto err;;
	}

	data->track = xmms_mp4_get_aac_track (data->mp4ff);
	if (data->track < 0) {
		XMMS_DBG ("Can't find AAC audio track from MP4 file\n");
		goto err;
	}
	data->numsamples = mp4ff_num_samples (data->mp4ff, data->track);
	mp4ff_get_decoder_config (data->mp4ff, data->track, &tmpbuf,
	                          &tmpbuflen);

	if (faacDecInit2 (data->decoder, tmpbuf, tmpbuflen,
	                  &samplerate, &channels) < 0) {
		XMMS_DBG ("Error initializing decoder library.");
		g_free (tmpbuf);
		goto err;
	}
	g_free (tmpbuf);

	data->samplerate = samplerate;
	data->channels = channels;

	xmms_mp4_get_mediainfo (xform);

	xmms_xform_outdata_type_add (xform,
	                             XMMS_STREAM_TYPE_MIMETYPE,
	                             "audio/pcm",
	                             XMMS_STREAM_TYPE_FMT_FORMAT,
	                             data->sampleformat,
	                             XMMS_STREAM_TYPE_FMT_CHANNELS,
	                             data->channels,
	                             XMMS_STREAM_TYPE_FMT_SAMPLERATE,
	                             data->samplerate,
	                             XMMS_STREAM_TYPE_END);

	XMMS_DBG ("AAC decoder inited successfully!");

	return TRUE;

err:
	g_free (data->mp4ff_cb);
	g_string_free (data->outbuf, TRUE);
	g_free (data);

	return FALSE;
}

static gint
xmms_mp4_read (xmms_xform_t *xform, xmms_sample_t *buf, gint len, xmms_error_t *err)
{
	xmms_mp4_data_t *data;

	faacDecFrameInfo frameInfo;
	gpointer sample_buffer;
	guint size, bytes_read = 0;

	data = xmms_xform_private_data_get (xform);
	g_return_val_if_fail (data, -1);

	size = MIN (data->outbuf->len, len);
	while (size == 0) {
		guchar *tmpbuf;
		guint tmpbuflen;
		gint duration, offset;

		if (data->sampleid >= data->numsamples) {
			XMMS_DBG ("MP4 EOF");
			return 0;
		}
		
		bytes_read = mp4ff_read_sample (data->mp4ff, data->track,
		                                data->sampleid, &tmpbuf,
		                                &tmpbuflen);
		duration = mp4ff_get_sample_duration (data->mp4ff, data->track,
		                                      data->sampleid);
		offset = mp4ff_get_sample_offset (data->mp4ff, data->track,
		                                  data->sampleid);
		data->sampleid++;

		sample_buffer = faacDecDecode (data->decoder, &frameInfo,
		                               tmpbuf, tmpbuflen);
		sample_buffer += offset;
		bytes_read = (duration - offset) * frameInfo.channels *
		             xmms_sample_size_get (data->sampleformat);
		g_free (tmpbuf);

		if (bytes_read > 0 && frameInfo.error == 0) {
			g_string_append_len (data->outbuf, sample_buffer + data->toskip,
			                     bytes_read - data->toskip);
			data->toskip = 0;
		} else if (frameInfo.error > 0) {
			XMMS_DBG ("ERROR in faad decoding: %s", faacDecGetErrorMessage (frameInfo.error));
			return -1;
		}

		size = MIN (data->outbuf->len, len);
	}

	memcpy (buf, data->outbuf->str, size);
	g_string_erase (data->outbuf, 0, size);
	return size;
}

static gint64
xmms_mp4_seek (xmms_xform_t *xform, gint64 samples, xmms_xform_seek_mode_t whence, xmms_error_t *err)
{
	int32_t toskip;
	xmms_mp4_data_t *data;
	
	g_return_val_if_fail (whence == XMMS_XFORM_SEEK_SET, -1);
	g_return_val_if_fail (xform, -1);

	data = xmms_xform_private_data_get (xform);
	g_return_val_if_fail (data, FALSE);

	data->sampleid = mp4ff_find_sample_use_offsets (data->mp4ff, data->track,
	                                                samples, &toskip);
	data->toskip = toskip * data->channels * xmms_sample_size_get (data->sampleformat);
	data->buffer_length = 0;

	return samples;
}

static void
xmms_mp4_get_mediainfo (xmms_xform_t *xform)
{
	xmms_mp4_data_t *data;
	glong temp;
	gchar *metabuf;

	g_return_if_fail (xform);

	data = xmms_xform_private_data_get (xform);
	g_return_if_fail (data);

	temp = mp4ff_get_sample_rate (data->mp4ff, data->track);
	xmms_xform_metadata_set_int (xform,
	                             XMMS_MEDIALIB_ENTRY_PROPERTY_SAMPLERATE,
	                             temp);
	if ((temp = mp4ff_get_track_duration_use_offsets (data->mp4ff, data->track) / temp) >= 0) {
		xmms_xform_metadata_set_int (xform,
		                             XMMS_MEDIALIB_ENTRY_PROPERTY_DURATION,
		                             temp * 1000);
	}
	if ((temp = mp4ff_get_avg_bitrate (data->mp4ff, data->track)) >= 0) {
		xmms_xform_metadata_set_int (xform,
		                             XMMS_MEDIALIB_ENTRY_PROPERTY_BITRATE,
		                             temp);
	}
	if (mp4ff_meta_get_artist (data->mp4ff, &metabuf)) {
		xmms_xform_metadata_set_str (xform,
		                             XMMS_MEDIALIB_ENTRY_PROPERTY_ARTIST,
		                             metabuf);
		g_free (metabuf);
	}
	if (mp4ff_meta_get_title (data->mp4ff, &metabuf)) {
		xmms_xform_metadata_set_str (xform,
		                             XMMS_MEDIALIB_ENTRY_PROPERTY_TITLE,
		                             metabuf);
		g_free (metabuf);
	}
	if (mp4ff_meta_get_album (data->mp4ff, &metabuf)) {
		xmms_xform_metadata_set_str (xform,
		                             XMMS_MEDIALIB_ENTRY_PROPERTY_ALBUM,
		                             metabuf);
		g_free (metabuf);
	}
	if (mp4ff_meta_get_date (data->mp4ff, &metabuf)) {
		xmms_xform_metadata_set_str (xform,
		                             XMMS_MEDIALIB_ENTRY_PROPERTY_YEAR,
		                             metabuf);
		g_free (metabuf);
	}
	if (mp4ff_meta_get_genre (data->mp4ff, &metabuf)) {
		xmms_xform_metadata_set_str (xform,
		                             XMMS_MEDIALIB_ENTRY_PROPERTY_GENRE,
		                             metabuf);
		g_free (metabuf);
	}
	if (mp4ff_meta_get_comment (data->mp4ff, &metabuf)) {
		xmms_xform_metadata_set_str (xform,
		                             XMMS_MEDIALIB_ENTRY_PROPERTY_COMMENT,
		                             metabuf);
		g_free (metabuf);
	}
	if (mp4ff_meta_get_track (data->mp4ff, &metabuf)) {
		gint tracknr;
		gchar *end;

		tracknr = strtol (metabuf, &end, 10);
		if (end && *end == '\0') {
			xmms_xform_metadata_set_int (xform,
			                             XMMS_MEDIALIB_ENTRY_PROPERTY_TRACKNR,
			                             tracknr);
		}
		g_free (metabuf);
	}
	if ((temp = mp4ff_meta_get_coverart (data->mp4ff, &metabuf))) {
		gchar hash[33];

		if (xmms_bindata_plugin_add ((guchar *) metabuf, temp, hash)) {
			xmms_xform_metadata_set_str (xform, XMMS_MEDIALIB_ENTRY_PROPERTY_PICTURE_FRONT, hash);
			xmms_xform_metadata_set_str (xform, XMMS_MEDIALIB_ENTRY_PROPERTY_PICTURE_FRONT_MIME, "image/jpeg");
		}
	}

	/* MusicBrainz tag support */
	if (mp4ff_meta_find_by_name (data->mp4ff, "MusicBrainz Track Id", &metabuf)) {
		xmms_xform_metadata_set_str (xform,
		                             XMMS_MEDIALIB_ENTRY_PROPERTY_TRACK_ID,
		                             metabuf);
		g_free (metabuf);
	}
	if (mp4ff_meta_find_by_name (data->mp4ff, "MusicBrainz Album Id", &metabuf)) {
		xmms_xform_metadata_set_str (xform,
		                             XMMS_MEDIALIB_ENTRY_PROPERTY_ALBUM_ID,
		                             metabuf);
		g_free (metabuf);
	}
	if (mp4ff_meta_find_by_name (data->mp4ff, "MusicBrainz Artist Id", &metabuf)) {
		xmms_xform_metadata_set_str (xform,
		                             XMMS_MEDIALIB_ENTRY_PROPERTY_ARTIST_ID,
		                             metabuf);
		g_free (metabuf);
	}

	/* Replay Gain support */
	if (mp4ff_meta_find_by_name (data->mp4ff, "replaygain_track_gain", &metabuf)) {
		gchar buf[8];

		g_snprintf (buf, sizeof (buf), "%f",
		            pow (10.0, g_strtod (metabuf, NULL) / 20));
		g_free (metabuf);

		xmms_xform_metadata_set_str (xform,
		                             XMMS_MEDIALIB_ENTRY_PROPERTY_GAIN_TRACK,
		                             buf);
	}
	if (mp4ff_meta_find_by_name (data->mp4ff, "replaygain_album_gain", &metabuf)) {
		gchar buf[8];

		g_snprintf (buf, sizeof (buf), "%f",
		            pow (10.0, g_strtod (metabuf, NULL) / 20));
		g_free (metabuf);

		xmms_xform_metadata_set_str (xform,
		                             XMMS_MEDIALIB_ENTRY_PROPERTY_GAIN_ALBUM,
		                             buf);
	}
	if (mp4ff_meta_find_by_name (data->mp4ff, "replaygain_track_peak", &metabuf)) {
		xmms_xform_metadata_set_str (xform,
		                             XMMS_MEDIALIB_ENTRY_PROPERTY_PEAK_TRACK,
		                             metabuf);
		g_free (metabuf);
	}
	if (mp4ff_meta_find_by_name (data->mp4ff, "replaygain_album_peak", &metabuf)) {
		xmms_xform_metadata_set_str (xform,
		                             XMMS_MEDIALIB_ENTRY_PROPERTY_PEAK_ALBUM,
		                             metabuf);
		g_free (metabuf);
	}
}

uint32_t
xmms_mp4_read_callback (void *user_data, void *buffer, uint32_t length)
{
	xmms_xform_t *xform;
	xmms_mp4_data_t *data;
	xmms_error_t error;
	gint ret;

	g_return_val_if_fail (user_data, 0);
	g_return_val_if_fail (buffer, 0);
	xform = user_data;

	data = xmms_xform_private_data_get (xform);
	g_return_val_if_fail (data, 0);

	if (data->buffer_length == 0) {
		guint bytes_read;

		bytes_read = xmms_xform_read (xform, (gchar *) data->buffer,
		                              data->buffer_size, &error);

		if (bytes_read <= 0 && data->buffer_length == 0) {
			return bytes_read;
		}

		data->buffer_length += bytes_read;
	}

	ret = MIN (length, data->buffer_length);
	g_memmove (buffer, data->buffer, ret);
	g_memmove (data->buffer, data->buffer + ret, data->buffer_length - ret);
	data->buffer_length -= ret;

	return ret;
}

uint32_t
xmms_mp4_seek_callback (void *user_data, uint64_t position)
{
	xmms_xform_t *xform;
	xmms_mp4_data_t *data;
	xmms_error_t error;
	gint ret = 0;

	g_return_val_if_fail (user_data, -1);
	xform = user_data;

	data = xmms_xform_private_data_get (xform);
	g_return_val_if_fail (data, -1);

	ret = xmms_xform_seek (xform, position, XMMS_XFORM_SEEK_SET, &error);

	/* If seeking was successfull, flush the internal buffer */
	if (ret >= 0) {
		data->buffer_length = 0;
	}

	return ret;
}

int
xmms_mp4_get_aac_track (mp4ff_t *infile)
{
	int i;
	int numTracks = mp4ff_total_tracks (infile);

	/* find first AAC audio track */
	for (i = 0; i < numTracks; i++) {
		gint object_type = mp4ff_get_audio_type (infile, i);

		/* these identifiers are mostly from VLC code */
		switch (object_type) {
		case 0x40: /* MPEG-4 audio */
		case 0x66: /* MPEG-2 AAC */
		case 0x67: /* MPEG-2 AAC LC */
		case 0x68: /* MPEG-2 AAC SSR */
			return i;
		case 0x69: /* MPEG-2 audio */
		case 0x6B: /* MPEG-1 audio */
			continue;
		case 0x00: /* ALAC audio, 0x00 sounds quite fishy... */
			continue;
		default:
			continue;
		}
	}

	/* can't decode this */
	return -1;
}

/*
 * Copyright (C) 2009-2011 Julien BLACHE <jb@jblache.org>
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA  02111-1307  USA
 */

#ifdef HAVE_CONFIG_H
# include <config.h>
#endif

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include <stdint.h>

#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/opt.h>

#include "db.h"
#include "logger.h"
#include "misc.h"
#include "http.h"

/* Mapping between the metadata name(s) and the offset
 * of the equivalent metadata field in struct media_file_info */
struct metadata_map {
  char *key;
  int as_int;
  size_t offset;
  int (*handler_function)(struct media_file_info *, char *);
};

// Used for passing errors to DPRINTF (can't count on av_err2str being present)
static char errbuf[64];

static inline char *
err2str(int errnum)
{
  av_strerror(errnum, errbuf, sizeof(errbuf));
  return errbuf;
}

static int
parse_slash_separated_ints(char *string, uint32_t *firstval, uint32_t *secondval)
{
  int numvals = 0;
  char *ptr;

  ptr = strchr(string, '/');
  if (ptr)
    {
      *ptr = '\0';
      if (safe_atou32(ptr + 1, secondval) == 0)
        numvals++;
    }

  if (safe_atou32(string, firstval) == 0)
    numvals++;

  return numvals;
}

static int
parse_track(struct media_file_info *mfi, char *track_string)
{
  uint32_t *track = (uint32_t *) ((char *) mfi + mfi_offsetof(track));
  uint32_t *total_tracks = (uint32_t *) ((char *) mfi + mfi_offsetof(total_tracks));

  return parse_slash_separated_ints(track_string, track, total_tracks);
}

static int
parse_disc(struct media_file_info *mfi, char *disc_string)
{
  uint32_t *disc = (uint32_t *) ((char *) mfi + mfi_offsetof(disc));
  uint32_t *total_discs = (uint32_t *) ((char *) mfi + mfi_offsetof(total_discs));

  return parse_slash_separated_ints(disc_string, disc, total_discs);
}

static int
parse_date(struct media_file_info *mfi, char *date_string)
{
  char year_string[32];
  uint32_t *year = (uint32_t *) ((char *) mfi + mfi_offsetof(year));
  uint32_t *date_released = (uint32_t *) ((char *) mfi + mfi_offsetof(date_released));
  struct tm tm = { 0 };
  int ret = 0;

  if ((*year == 0) && (safe_atou32(date_string, year) == 0))
    ret++;

  if ( strptime(date_string, "%FT%T%z", &tm) // ISO 8601, %F=%Y-%m-%d, %T=%H:%M:%S
       || strptime(date_string, "%F %T", &tm)
       || strptime(date_string, "%F %H:%M", &tm)
       || strptime(date_string, "%F", &tm)
     )
    {
      *date_released = (uint32_t)mktime(&tm);
      ret++;
    }

  if ((*date_released == 0) && (*year != 0))
    {
      snprintf(year_string, sizeof(year_string), "%" PRIu32 "-01-01T12:00:00", *year);
      if (strptime(year_string, "%FT%T", &tm))
	{
	  *date_released = (uint32_t)mktime(&tm);
	  ret++;
	}
    }

  return ret;
}

static int
parse_albumid(struct media_file_info *mfi, char *id_string)
{
  // Already set by a previous tag that we give higher priority
  if (mfi->songalbumid)
    return 0;

  // Limit hash length to 63 bits, due to signed type in sqlite
  mfi->songalbumid = murmur_hash64(id_string, strlen(id_string), 0) >> 1;
  return 1;
}

/* Lookup is case-insensitive, first occurrence takes precedence */
static const struct metadata_map md_map_generic[] =
  {
    { "title",        0, mfi_offsetof(title),              NULL },
    { "artist",       0, mfi_offsetof(artist),             NULL },
    { "author",       0, mfi_offsetof(artist),             NULL },
    { "album_artist", 0, mfi_offsetof(album_artist),       NULL },
    { "album",        0, mfi_offsetof(album),              NULL },
    { "genre",        0, mfi_offsetof(genre),              NULL },
    { "composer",     0, mfi_offsetof(composer),           NULL },
    { "grouping",     0, mfi_offsetof(grouping),           NULL },
    { "orchestra",    0, mfi_offsetof(orchestra),          NULL },
    { "conductor",    0, mfi_offsetof(conductor),          NULL },
    { "comment",      0, mfi_offsetof(comment),            NULL },
    { "description",  0, mfi_offsetof(comment),            NULL },
    { "track",        1, mfi_offsetof(track),              parse_track },
    { "disc",         1, mfi_offsetof(disc),               parse_disc },
    { "year",         1, mfi_offsetof(year),               NULL },
    { "date",         1, mfi_offsetof(date_released),      parse_date },
    { "title-sort",   0, mfi_offsetof(title_sort),         NULL },
    { "artist-sort",  0, mfi_offsetof(artist_sort),        NULL },
    { "album-sort",   0, mfi_offsetof(album_sort),         NULL },
    { "compilation",  1, mfi_offsetof(compilation),        NULL },

    // ALAC sort tags
    { "sort_name",           0, mfi_offsetof(title_sort),         NULL },
    { "sort_artist",         0, mfi_offsetof(artist_sort),        NULL },
    { "sort_album",          0, mfi_offsetof(album_sort),         NULL },
    { "sort_album_artist",   0, mfi_offsetof(album_artist_sort),  NULL },
    { "sort_composer",       0, mfi_offsetof(composer_sort),      NULL },

    // These tags are used to determine if files belong to a common compilation
    // or album, ref. https://picard.musicbrainz.org/docs/tags
    { "MusicBrainz Album Id",         1, mfi_offsetof(songalbumid), parse_albumid },
    { "MUSICBRAINZ_ALBUMID",          1, mfi_offsetof(songalbumid), parse_albumid },
    { "MusicBrainz Release Group Id", 1, mfi_offsetof(songalbumid), parse_albumid },
    { "MusicBrainz DiscID",           1, mfi_offsetof(songalbumid), parse_albumid },
    { "CDDB DiscID",                  1, mfi_offsetof(songalbumid), parse_albumid },
    { "CATALOGNUMBER",                1, mfi_offsetof(songalbumid), parse_albumid },
    { "BARCODE",                      1, mfi_offsetof(songalbumid), parse_albumid },

    { NULL,           0, 0,                                NULL }
  };

static const struct metadata_map md_map_tv[] =
  {
    { "stik",         1, mfi_offsetof(media_kind),         NULL },
    { "show",         0, mfi_offsetof(tv_series_name),     NULL },
    { "episode_id",   0, mfi_offsetof(tv_episode_num_str), NULL },
    { "network",      0, mfi_offsetof(tv_network_name),    NULL },
    { "episode_sort", 1, mfi_offsetof(tv_episode_sort),    NULL },
    { "season_number",1, mfi_offsetof(tv_season_num),      NULL },

    { NULL,           0, 0,                                NULL }
  };

/* NOTE about VORBIS comments:
 *  Only a small set of VORBIS comment fields are officially designated. Most
 *  common tags are at best de facto standards. Currently, metadata conversion
 *  functionality in ffmpeg only adds support for a couple of tags. Specifically,
 *  ALBUMARTIST and TRACKNUMBER are handled as of Feb 1, 2010 (rev 21587). Tags
 *  with names that already match the generic ffmpeg scheme--TITLE and ARTIST,
 *  for example--are of course handled. The rest of these tags are reported to
 *  have been used by various programs in the wild.
 */
static const struct metadata_map md_map_vorbis[] =
  {
    { "albumartist",  0, mfi_offsetof(album_artist),      NULL },
    { "album artist", 0, mfi_offsetof(album_artist),      NULL },
    { "tracknumber",  1, mfi_offsetof(track),             NULL },
    { "tracktotal",   1, mfi_offsetof(total_tracks),      NULL },
    { "totaltracks",  1, mfi_offsetof(total_tracks),      NULL },
    { "discnumber",   1, mfi_offsetof(disc),              NULL },
    { "disctotal",    1, mfi_offsetof(total_discs),       NULL },
    { "totaldiscs",   1, mfi_offsetof(total_discs),       NULL },

    { NULL,           0, 0,                               NULL }
  };

/* NOTE about ID3 tag names:
 *  metadata conversion for ID3v2 tags was added in ffmpeg in september 2009
 *  (rev 20073) for ID3v2.3; support for ID3v2.2 tag names was added in december
 *  2009 (rev 20839).
 *
 * ID3v2.x tags will be removed from the map once a version of ffmpeg containing
 * the changes listed above will be generally available. The more entries in the
 * map, the slower the filescanner gets.
 *
 * Update 20180131: Removed tags supported by ffmpeg 2.5.4 (around 3 years old)
 * + added some tags used for grouping
 * Update 20200114: Removed TDA, TDAT, TYE, TYER, TDR since the they are
 * well supported by ffmpeg, and the server was parsing TDA/TDAT incorrectly
 *
 */
static const struct metadata_map md_map_id3[] =
  {
    { "TT1",                 0, mfi_offsetof(grouping),              NULL },              /* ID3v2.2 */
    { "TIT1",                0, mfi_offsetof(grouping),              NULL },              /* ID3v2.3 */
    { "GP1",                 0, mfi_offsetof(grouping),              NULL },              /* unofficial iTunes */
    { "GRP1",                0, mfi_offsetof(grouping),              NULL },              /* unofficial iTunes */
    { "TCM",                 0, mfi_offsetof(composer),              NULL },              /* ID3v2.2 */
    { "TPA",                 1, mfi_offsetof(disc),                  parse_disc },        /* ID3v2.2 */
    { "XSOA",                0, mfi_offsetof(album_sort),            NULL },              /* ID3v2.3 */
    { "XSOP",                0, mfi_offsetof(artist_sort),           NULL },              /* ID3v2.3 */
    { "XSOT",                0, mfi_offsetof(title_sort),            NULL },              /* ID3v2.3 */
    { "TS2",                 0, mfi_offsetof(album_artist_sort),     NULL },              /* ID3v2.2 */
    { "TSO2",                0, mfi_offsetof(album_artist_sort),     NULL },              /* ID3v2.3 */
    { "ALBUMARTISTSORT",     0, mfi_offsetof(album_artist_sort),     NULL },              /* ID3v2.x */
    { "TSC",                 0, mfi_offsetof(composer_sort),         NULL },              /* ID3v2.2 */
    { "TSOC",                0, mfi_offsetof(composer_sort),         NULL },              /* ID3v2.3 */

    { NULL,                  0, 0,                                   NULL }
  };


static int
extract_metadata_core(struct media_file_info *mfi, AVDictionary *md, const struct metadata_map *md_map)
{
  AVDictionaryEntry *mdt;
  char **strval;
  uint32_t *intval;
  int mdcount;
  int i;
  int ret;

#if 0
  /* Dump all the metadata reported by ffmpeg */
  mdt = NULL;
  while ((mdt = av_dict_get(md, "", mdt, AV_DICT_IGNORE_SUFFIX)) != NULL)
    DPRINTF(E_DBG, L_SCAN, " -> %s = %s\n", mdt->key, mdt->value);
#endif

  mdcount = 0;

  /* Extract actual metadata */
  for (i = 0; md_map[i].key != NULL; i++)
    {
      mdt = av_dict_get(md, md_map[i].key, NULL, 0);
      if (mdt == NULL)
	continue;

      if ((mdt->value == NULL) || (strlen(mdt->value) == 0))
	continue;

      if (md_map[i].handler_function)
	{
	  mdcount += md_map[i].handler_function(mfi, mdt->value);
	  continue;
	}

      mdcount++;

      if (!md_map[i].as_int)
	{
	  strval = (char **) ((char *) mfi + md_map[i].offset);

	  if (*strval == NULL)
	    *strval = strdup(mdt->value);
	}
      else
	{
	  intval = (uint32_t *) ((char *) mfi + md_map[i].offset);

	  if (*intval == 0)
	    {
	      ret = safe_atou32(mdt->value, intval);
	      if (ret < 0)
		continue;
	    }
	}
    }

  return mdcount;
}

static int
extract_metadata(struct media_file_info *mfi, AVFormatContext *ctx, AVStream *audio_stream, AVStream *video_stream, const struct metadata_map *md_map)
{
  int mdcount;
  int ret;

  mdcount = 0;

  if (ctx->metadata)
    {
      ret = extract_metadata_core(mfi, ctx->metadata, md_map);
      mdcount += ret;

      DPRINTF(E_DBG, L_SCAN, "Picked up %d tags from file metadata\n", ret);
    }

  if (audio_stream->metadata)
    {
      ret = extract_metadata_core(mfi, audio_stream->metadata, md_map);
      mdcount += ret;

      DPRINTF(E_DBG, L_SCAN, "Picked up %d tags from audio stream metadata\n", ret);
    }

  if (video_stream && video_stream->metadata)
    {
      ret = extract_metadata_core(mfi, video_stream->metadata, md_map);
      mdcount += ret;

      DPRINTF(E_DBG, L_SCAN, "Picked up %d tags from video stream metadata\n", ret);
    }

  return mdcount;
}

/*
 * Fills metadata read with ffmpeg/libav from the given path into the given mfi
 *
 * Following attributes from the given mfi are read to control how to read metadata:
 * - data_kind: if data_kind is http, icy metadata is used, if the path points to a playlist the first stream-uri in that playlist is used
 * - media_kind: if media_kind is podcast or audiobook, video streams in the file are ignored
 * - compilation: like podcast/audiobook video streams are ignored for compilations
 * - file_size: if bitrate could not be read through ffmpeg/libav, file_size is used to estimate the bitrate
 * - fname: (filename) used as fallback for artist
 */
int
scan_metadata_ffmpeg(struct media_file_info *mfi, const char *file)
{
  AVFormatContext *ctx;
  AVDictionary *options;
  const struct metadata_map *extra_md_map;
  struct http_icy_metadata *icy_metadata;
  enum AVMediaType codec_type;
  enum AVCodecID codec_id;
  enum AVCodecID video_codec_id;
  enum AVCodecID audio_codec_id;
  enum AVSampleFormat sample_fmt;
  AVStream *video_stream;
  AVStream *audio_stream;
  char *path;
  int mdcount;
  int sample_rate;
  int channels;
  int i;
  int ret;

  ctx = NULL;
  options = NULL;
  path = strdup(file);

  if (mfi->data_kind == DATA_KIND_HTTP)
    {
#ifndef HAVE_FFMPEG
      // Without this, libav is slow to probe some internet streams
      ctx = avformat_alloc_context();
      ctx->probesize = 64000;
#endif

      free(path);
      ret = http_stream_setup(&path, file);
      if (ret < 0)
	return -1;

      av_dict_set(&options, "icy", "1", 0);
    }
  else if (mfi->data_kind == DATA_KIND_FILE && mfi->file_size == 0)
    {
      // a 0-byte mp3 will make ffmpeg die with arithmetic exception (with 3.2.15-0+deb9u4)
      free(path);
      return -1;
    }

  ret = avformat_open_input(&ctx, path, NULL, &options);

  if (options)
    av_dict_free(&options);

  if (ret != 0)
    {
      DPRINTF(E_WARN, L_SCAN, "Cannot open media file '%s': %s\n", path, err2str(ret));

      free(path);
      return -1;
    }

  ret = avformat_find_stream_info(ctx, NULL);
  if (ret < 0)
    {
      DPRINTF(E_WARN, L_SCAN, "Cannot get stream info of '%s': %s\n", path, err2str(ret));

      avformat_close_input(&ctx);
      free(path);
      return -1;
    }

  free(path);

#if 0
  /* Dump input format as determined by ffmpeg */
  av_dump_format(ctx, 0, file, 0);
#endif

  DPRINTF(E_DBG, L_SCAN, "File has %d streams\n", ctx->nb_streams);

  /* Extract codec IDs, check for video */
  video_codec_id = AV_CODEC_ID_NONE;
  video_stream = NULL;

  audio_codec_id = AV_CODEC_ID_NONE;
  audio_stream = NULL;

  for (i = 0; i < ctx->nb_streams; i++)
    {
      codec_type = ctx->streams[i]->codecpar->codec_type;
      codec_id = ctx->streams[i]->codecpar->codec_id;
      sample_rate = ctx->streams[i]->codecpar->sample_rate;
      sample_fmt = ctx->streams[i]->codecpar->format;
// Matches USE_CH_LAYOUT in transcode.c
#if (LIBAVCODEC_VERSION_MAJOR > 59) || ((LIBAVCODEC_VERSION_MAJOR == 59) && (LIBAVCODEC_VERSION_MINOR > 24))
      channels = ctx->streams[i]->codecpar->ch_layout.nb_channels;
#else
      channels = ctx->streams[i]->codecpar->channels;
#endif
      switch (codec_type)
	{
	  case AVMEDIA_TYPE_VIDEO:
	    if (ctx->streams[i]->disposition & AV_DISPOSITION_ATTACHED_PIC)
	      {
		DPRINTF(E_DBG, L_SCAN, "Found embedded artwork (stream %d)\n", i);
		mfi->artwork = ARTWORK_EMBEDDED;

		break;
	      }

	    // We treat these as audio no matter what
	    if (mfi->compilation || (mfi->media_kind & (MEDIA_KIND_PODCAST | MEDIA_KIND_AUDIOBOOK)))
	      break;

	    if (!video_stream)
	      {
		DPRINTF(E_DBG, L_SCAN, "File has video (stream %d)\n", i);

		video_stream = ctx->streams[i];
		video_codec_id = codec_id;

		mfi->has_video = 1;
	      }
	    break;

	  case AVMEDIA_TYPE_AUDIO:
	    if (!audio_stream)
	      {
		audio_stream = ctx->streams[i];
		audio_codec_id = codec_id;

		mfi->samplerate = sample_rate;
		mfi->bits_per_sample = 8 * av_get_bytes_per_sample(sample_fmt);
		if (mfi->bits_per_sample == 0)
		  mfi->bits_per_sample = av_get_bits_per_sample(codec_id);
		mfi->channels = channels;
	      } 
	    break;

	  default:
	    break;
	}
    }

  if (audio_codec_id == AV_CODEC_ID_NONE)
    {
      DPRINTF(E_DBG, L_SCAN, "File has no audio streams, discarding\n");

      avformat_close_input(&ctx);
      return -1;
    }

  /* Common media information */
  if (ctx->duration > 0)
    mfi->song_length = ctx->duration / (AV_TIME_BASE / 1000); /* ms */

  if (ctx->bit_rate > 0)
    mfi->bitrate = ctx->bit_rate / 1000;
  else if (ctx->duration > AV_TIME_BASE) /* guesstimate */
    mfi->bitrate = ((mfi->file_size * 8) / (ctx->duration / AV_TIME_BASE)) / 1000;

  DPRINTF(E_DBG, L_SCAN, "Duration %d ms, bitrate %d kbps, samplerate %d channels %d\n", mfi->song_length, mfi->bitrate, mfi->samplerate, mfi->channels);

  /* Try to extract ICY metadata if http stream */
  if (mfi->data_kind == DATA_KIND_HTTP)
    {
      icy_metadata = http_icy_metadata_get(ctx, 0);
      if (icy_metadata && icy_metadata->name)
	{
	  DPRINTF(E_DBG, L_SCAN, "Found ICY metadata, name is '%s'\n", icy_metadata->name);

	  if (mfi->title)
	    free(mfi->title);
	  if (mfi->artist)
	    free(mfi->artist);
	  if (mfi->album_artist)
	    free(mfi->album_artist);

	  mfi->title = strdup(icy_metadata->name);
	  mfi->artist = strdup(icy_metadata->name);
	  mfi->album_artist = strdup(icy_metadata->name);
	}
      if (icy_metadata && icy_metadata->description)
	{
	  DPRINTF(E_DBG, L_SCAN, "Found ICY metadata, description is '%s'\n", icy_metadata->description);

	  if (mfi->album)
	    free(mfi->album);

	  mfi->album = strdup(icy_metadata->description);
	}
      if (icy_metadata && icy_metadata->genre)
	{
	  DPRINTF(E_DBG, L_SCAN, "Found ICY metadata, genre is '%s'\n", icy_metadata->genre);

	  if (mfi->genre)
	    free(mfi->genre);

	  mfi->genre = strdup(icy_metadata->genre);
	}
      if (icy_metadata)
	http_icy_metadata_free(icy_metadata, 0);
    }

  /* Check codec */
  extra_md_map = NULL;
  codec_id = (mfi->has_video) ? video_codec_id : audio_codec_id;
  switch (codec_id)
    {
      case AV_CODEC_ID_AAC:
	DPRINTF(E_DBG, L_SCAN, "AAC\n");
	mfi->type = strdup("m4a");
	mfi->codectype = strdup("mp4a");
	mfi->description = strdup("AAC audio file");
	break;

      case AV_CODEC_ID_ALAC:
	DPRINTF(E_DBG, L_SCAN, "ALAC\n");
	mfi->type = strdup("m4a");
	mfi->codectype = strdup("alac");
	mfi->description = strdup("Apple Lossless audio file");
	break;

      case AV_CODEC_ID_FLAC:
	DPRINTF(E_DBG, L_SCAN, "FLAC\n");
	mfi->type = strdup("flac");
	mfi->codectype = strdup("flac");
	mfi->description = strdup("FLAC audio file");

	extra_md_map = md_map_vorbis;
	break;

      case AV_CODEC_ID_APE:
	DPRINTF(E_DBG, L_SCAN, "APE\n");
	mfi->type = strdup("ape");
	mfi->codectype = strdup("ape");
	mfi->description = strdup("Monkey's audio");
	break;

      case AV_CODEC_ID_MUSEPACK7:
      case AV_CODEC_ID_MUSEPACK8:
	DPRINTF(E_DBG, L_SCAN, "Musepack\n");
	mfi->type = strdup("mpc");
	mfi->codectype = strdup("mpc");
	mfi->description = strdup("Musepack audio file");
	break;

      case AV_CODEC_ID_MPEG4: /* Video */
      case AV_CODEC_ID_H264:
	DPRINTF(E_DBG, L_SCAN, "MPEG4 video\n");
	mfi->type = strdup("m4v");
	mfi->codectype = strdup("mp4v");
	mfi->description = strdup("MPEG-4 video file");

	extra_md_map = md_map_tv;
	break;

      case AV_CODEC_ID_MP3:
	DPRINTF(E_DBG, L_SCAN, "MP3\n");
	mfi->type = strdup("mp3");
	mfi->codectype = strdup("mpeg");
	mfi->description = strdup("MPEG audio file");

	extra_md_map = md_map_id3;
	break;

      case AV_CODEC_ID_VORBIS:
	DPRINTF(E_DBG, L_SCAN, "VORBIS\n");
	mfi->type = strdup("ogg");
	mfi->codectype = strdup("ogg");
	mfi->description = strdup("Ogg Vorbis audio file");

	extra_md_map = md_map_vorbis;
	break;

      case AV_CODEC_ID_WMAV1:
      case AV_CODEC_ID_WMAV2:
      case AV_CODEC_ID_WMAVOICE:
	DPRINTF(E_DBG, L_SCAN, "WMA Voice\n");
	mfi->type = strdup("wma");
	mfi->codectype = strdup("wmav");
	mfi->description = strdup("WMA audio file");
	break;

      case AV_CODEC_ID_WMAPRO:
	DPRINTF(E_DBG, L_SCAN, "WMA Pro\n");
	mfi->type = strdup("wmap");
	mfi->codectype = strdup("wma");
	mfi->description = strdup("WMA audio file");
	break;

      case AV_CODEC_ID_WMALOSSLESS:
	DPRINTF(E_DBG, L_SCAN, "WMA Lossless\n");
	mfi->type = strdup("wma");
	mfi->codectype = strdup("wmal");
	mfi->description = strdup("WMA audio file");
	break;

      case AV_CODEC_ID_PCM_S16LE ... AV_CODEC_ID_PCM_F64LE:
	if (strcmp(ctx->iformat->name, "aiff") == 0)
	  {
	    DPRINTF(E_DBG, L_SCAN, "AIFF\n");
	    mfi->type = strdup("aif");
	    mfi->codectype = strdup("aif");
	    mfi->description = strdup("AIFF audio file");
	    break;
	  }
	else if (strcmp(ctx->iformat->name, "wav") == 0)
	  {
	    DPRINTF(E_DBG, L_SCAN, "WAV\n");
	    mfi->type = strdup("wav");
	    mfi->codectype = strdup("wav");
	    mfi->description = strdup("WAV audio file");
	    break;
	  }
	/* WARNING: will fallthrough to default case, don't move */
	/* FALLTHROUGH */

      default:
	DPRINTF(E_DBG, L_SCAN, "Unknown codec 0x%x (video: %s), format %s (%s)\n",
		codec_id, (mfi->has_video) ? "yes" : "no", ctx->iformat->name, ctx->iformat->long_name);
	mfi->type = strdup("unkn");
	mfi->codectype = strdup("unkn");
	if (mfi->has_video)
	  {
	    mfi->description = strdup("Unknown video file format");
	    extra_md_map = md_map_tv;
	  }
	else
	  mfi->description = strdup("Unknown audio file format");
	break;
    }

  mdcount = 0;

  if ((!ctx->metadata) && (!audio_stream->metadata)
      && (video_stream && !video_stream->metadata))
    {
      DPRINTF(E_WARN, L_SCAN, "ffmpeg reports no metadata\n");

      goto skip_extract;
    }

  if (extra_md_map)
    {
      ret = extract_metadata(mfi, ctx, audio_stream, video_stream, extra_md_map);
      mdcount += ret;

      DPRINTF(E_DBG, L_SCAN, "Picked up %d tags with extra md_map\n", ret);
    }

  ret = extract_metadata(mfi, ctx, audio_stream, video_stream, md_map_generic);
  mdcount += ret;

  DPRINTF(E_DBG, L_SCAN, "Picked up %d tags with generic md_map, %d tags total\n", ret, mdcount);

  /* fix up TV metadata */
  if (mfi->media_kind == 10)
    {
      /* I have no idea why this is, but iTunes reports a media kind of 64 for stik==10 (?!) */
      mfi->media_kind = MEDIA_KIND_TVSHOW;
    }
  /* Unspecified video files are "Movies", media_kind 2 */
  else if (mfi->has_video == 1)
    {
      mfi->media_kind = MEDIA_KIND_MOVIE;
    }

 skip_extract:
  avformat_close_input(&ctx);

  if (mdcount == 0)
    DPRINTF(E_WARN, L_SCAN, "ffmpeg/libav could not extract any metadata\n");

  /* Just in case there's no title set ... */
  if (mfi->title == NULL)
    mfi->title = strdup(mfi->fname);

  /* All done */

  return 0;
}

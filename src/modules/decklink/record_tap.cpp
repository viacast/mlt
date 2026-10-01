/*
 * record_tap.cpp -- writer side of the Playcast recording tap (see record_tap.h)
 */

#include "record_tap.h"

#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

struct pctap_writer
{
	char name[64];
	pctap_header *header;
	size_t size;
};

static uint64_t now_us()
{
	struct timespec ts;
	clock_gettime( CLOCK_MONOTONIC, &ts );
	return (uint64_t) ts.tv_sec * 1000000 + ts.tv_nsec / 1000;
}

pctap_writer *pctap_open( const char *name, int width, int height, int fps_num, int fps_den,
	int progressive, int top_field_first, int audio_rate, int audio_channels )
{
	if ( !name || !*name || width <= 0 || height <= 0
		|| audio_channels <= 0 || audio_channels > PCTAP_MAX_CHANNELS )
		return NULL;

	pctap_writer *tap = (pctap_writer *) calloc( 1, sizeof( pctap_writer ) );
	if ( !tap )
		return NULL;
	// shm_open wants a single leading slash
	snprintf( tap->name, sizeof( tap->name ), "/%s", name[0] == '/' ? name + 1 : name );

	uint32_t stride = width * 2;
	uint32_t video_slot = sizeof( pctap_video_slot ) + stride * height;
	uint32_t audio_slot = sizeof( pctap_audio_slot )
		+ PCTAP_MAX_AUDIO_SAMPLES * audio_channels * sizeof( int16_t );
	video_slot = ( video_slot + 63 ) & ~63u;
	audio_slot = ( audio_slot + 63 ) & ~63u;
	uint64_t header_size = ( sizeof( pctap_header ) + 4095 ) & ~4095ull;
	tap->size = header_size + (uint64_t) PCTAP_SLOTS * ( video_slot + audio_slot );

	int fd = shm_open( tap->name, O_RDWR | O_CREAT, 0644 );
	if ( fd < 0 )
		goto fail;
	if ( ftruncate( fd, tap->size ) )
	{
		close( fd );
		goto fail;
	}
	tap->header = (pctap_header *) mmap( NULL, tap->size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0 );
	close( fd );
	if ( tap->header == MAP_FAILED )
	{
		tap->header = NULL;
		goto fail;
	}

	{
		pctap_header *h = tap->header;
		memset( h, 0, header_size );
		h->version = PCTAP_VERSION;
		h->header_size = sizeof( pctap_header );
		h->width = width;
		h->height = height;
		h->stride = stride;
		h->fps_num = fps_num;
		h->fps_den = fps_den;
		h->progressive = progressive;
		h->top_field_first = top_field_first;
		h->audio_rate = audio_rate;
		h->audio_channels = audio_channels;
		h->slots = PCTAP_SLOTS;
		h->video_slot_size = video_slot;
		h->audio_slot_size = audio_slot;
		h->video_offset = header_size;
		h->audio_offset = header_size + (uint64_t) PCTAP_SLOTS * video_slot;
		h->writer_pid = getpid();
		h->active = 1;
		h->heartbeat_us = now_us();
		__sync_synchronize();
		h->magic = PCTAP_MAGIC; // last: readers wait for it
	}
	return tap;

fail:
	shm_unlink( tap->name );
	free( tap );
	return NULL;
}

void pctap_write_video( pctap_writer *tap, uint64_t frame, const uint8_t *image, int repeat, uint32_t flags )
{
	pctap_header *h = tap->header;
	pctap_video_slot *slot = pctap_video( h, frame );
	slot->frame = 0;
	__sync_synchronize();
	slot->repeat = repeat || !image;
	slot->flags = flags;
	if ( !slot->repeat )
		memcpy( slot + 1, image, (size_t) h->stride * h->height );
	__sync_synchronize();
	slot->frame = frame + 1;
	h->video_next = frame + 1; // latest, not max: the frame count can restart
	h->heartbeat_us = now_us();
}

void pctap_write_audio( pctap_writer *tap, uint64_t frame, const int16_t *pcm, int samples, int channels )
{
	pctap_header *h = tap->header;
	if ( samples < 0 )
		samples = 0;
	if ( samples > PCTAP_MAX_AUDIO_SAMPLES )
		samples = PCTAP_MAX_AUDIO_SAMPLES;
	pctap_audio_slot *slot = pctap_audio( h, frame );
	slot->frame = 0;
	__sync_synchronize();
	slot->samples = samples;
	slot->channels = h->audio_channels;
	int16_t *dst = (int16_t *) ( slot + 1 );
	if ( channels == (int) h->audio_channels )
		memcpy( dst, pcm, (size_t) samples * channels * sizeof( int16_t ) );
	else
		for ( int s = 0; s < samples; s++ )
			for ( uint32_t c = 0; c < h->audio_channels; c++ )
				*dst++ = (int) c < channels ? pcm[s * channels + c] : 0;
	__sync_synchronize();
	slot->frame = frame + 1;
	h->audio_next = frame + 1; // latest, not max: the frame count can restart
}

void pctap_set_counters( pctap_writer *tap, uint64_t late, uint64_t dropped )
{
	tap->header->frames_late = late;
	tap->header->frames_dropped = dropped;
}

const char *pctap_name( pctap_writer *tap )
{
	return tap->name + 1;
}

void pctap_close( pctap_writer *tap )
{
	if ( !tap )
		return;
	if ( tap->header )
	{
		tap->header->active = 0;
		__sync_synchronize();
		munmap( tap->header, tap->size );
	}
	// readers keep their mapping until they unmap it themselves
	shm_unlink( tap->name );
	free( tap );
}

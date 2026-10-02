/*
 * producer_pcingest.c -- live network sources through playcast-live-gateway
 *
 * Resource: pcingest:<name>, the gateway's shared memory (/dev/shm/<name>,
 * see pcingest.h). The gateway does all the network and decoding work on
 * its own clock and always writes one frame per period, so this producer
 * never blocks: each get_frame takes the next frame from the ring, keeping
 * PCINGEST_LATENCY frames behind the newest one.
 *
 * - The gateway ahead of the output clock (the two clocks drift): one frame
 *   is skipped to get back to the target latency.
 * - Nothing new yet (the gateway behind): the last image is repeated, with
 *   silence.
 * - No gateway (not running, stalled, or not matching the profile): black
 *   and silence; the ring is reopened every second.
 *
 * Like the decklink and ndi producers, it's "infinite" (length
 * MLT_DEFAULT_LIVE_SOURCE_LENGTH, ~24 h at 29.97), which is how the
 * middleware recognizes live events.
 */

#include <framework/mlt.h>

#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#include "pcingest.h"

#define PCINGEST_LATENCY 2         /* frames kept behind the newest */
#define PCINGEST_STALE_US 2000000  /* no write for this long: gateway gone */
#define PCINGEST_REOPEN_US 1000000

typedef struct
{
	mlt_producer parent;
	char name[PCINGEST_NAME_MAX + 1];
	pthread_mutex_t lock;
	int fd;
	pcingest_header *ring;
	size_t ring_size;
	int64_t next_open_us;
	int have_cursor;
	uint64_t cursor;
	uint8_t *last_image;         /* the last image delivered, for repeats */
	size_t image_size;
	int last_state;              /* for logging transitions */
	uint64_t delivered, repeats, skips, blacks;
} producer_pcingest_t;

enum { STATE_NONE, STATE_BLACK, STATE_LIVE };

static int64_t now_us( void )
{
	struct timespec ts;
	clock_gettime( CLOCK_MONOTONIC, &ts );
	return (int64_t) ts.tv_sec * 1000000 + ts.tv_nsec / 1000;
}

static void close_ring( producer_pcingest_t *self )
{
	if ( self->ring )
		munmap( self->ring, self->ring_size );
	if ( self->fd >= 0 )
		close( self->fd );
	self->ring = NULL;
	self->fd = -1;
	self->have_cursor = 0;
}

/* Opens /dev/shm/<name> if it's a ring for this profile. */
static void open_ring( producer_pcingest_t *self, mlt_profile profile )
{
	struct stat st;
	char path[PCINGEST_NAME_MAX + 2];
	pcingest_header *h;

	snprintf( path, sizeof( path ), "/%s", self->name );
	self->fd = shm_open( path, O_RDONLY, 0 );
	if ( self->fd < 0 )
		return;
	if ( fstat( self->fd, &st ) || st.st_size < (off_t) sizeof( pcingest_header ) )
		goto fail;
	h = mmap( NULL, st.st_size, PROT_READ, MAP_SHARED, self->fd, 0 );
	if ( h == MAP_FAILED )
		goto fail;
	self->ring = h;
	self->ring_size = st.st_size;
	if ( h->magic != PCINGEST_MAGIC || h->version != PCINGEST_VERSION
		|| h->header_size != sizeof( pcingest_header )
		|| h->slots_offset + (uint64_t) h->slots * h->slot_size > (uint64_t) st.st_size
		|| h->audio_channels < 1 || h->audio_channels > PCINGEST_MAX_CHANNELS
		|| h->stride < h->width * 2 )
	{
		mlt_log_error( MLT_PRODUCER_SERVICE( self->parent ), "pcingest:%s: not a valid ring\n", self->name );
		goto fail;
	}
	if ( (int) h->width != profile->width || (int) h->height != profile->height
		|| (int64_t) h->fps_num * profile->frame_rate_den != (int64_t) profile->frame_rate_num * h->fps_den )
	{
		mlt_log_error( MLT_PRODUCER_SERVICE( self->parent ),
			"pcingest:%s: ring is %ux%u@%u/%u, the profile %dx%d@%d/%d\n", self->name,
			h->width, h->height, h->fps_num, h->fps_den,
			profile->width, profile->height, profile->frame_rate_num, profile->frame_rate_den );
		goto fail;
	}
	mlt_log_info( MLT_PRODUCER_SERVICE( self->parent ), "pcingest:%s: opened (%s)\n", self->name, h->input );
	return;
fail:
	close_ring( self );
}

static int ring_alive( producer_pcingest_t *self )
{
	pcingest_header *h = self->ring;
	if ( !h || !h->next )
		return 0;
	if ( h->writer_pid > 0 && kill( h->writer_pid, 0 ) && errno == ESRCH )
		return 0;
	return now_us() - (int64_t) h->heartbeat_us < PCINGEST_STALE_US;
}

/* Copies a complete slot (seqlock). Returns its flags, or -1 if it was
 * overwritten while copying. */
static int read_slot( producer_pcingest_t *self, uint64_t frame, uint8_t *image, int16_t *audio, int *samples )
{
	pcingest_header *h = self->ring;
	pcingest_slot *slot = pcingest_slot_at( h, frame );
	uint64_t before = slot->frame;
	uint32_t flags, n;

	if ( before != frame + 1 )
		return -1;
	__sync_synchronize();
	flags = slot->flags;
	n = slot->samples;
	if ( n > PCINGEST_MAX_AUDIO_SAMPLES )
		n = PCINGEST_MAX_AUDIO_SAMPLES;
	memcpy( image, pcingest_image( slot ), (size_t) h->stride * h->height );
	memcpy( audio, pcingest_audio( h, slot ), (size_t) n * h->audio_channels * 2 );
	__sync_synchronize();
	if ( slot->frame != before )
		return -1;
	*samples = n;
	return flags;
}

static void fill_black( uint8_t *image, size_t size )
{
	size_t i;
	/* YUYV black: Y=16, U=V=128 */
	for ( i = 0; i + 1 < size; i += 2 )
	{
		image[i] = 16;
		image[i + 1] = 128;
	}
}

static int get_image( mlt_frame frame, uint8_t **buffer, mlt_image_format *format, int *width, int *height, int writable )
{
	mlt_properties props = MLT_FRAME_PROPERTIES( frame );
	int size = 0;
	uint8_t *image = mlt_properties_get_data( props, "pcingest.image", &size );

	if ( !image )
		return 1;
	*format = mlt_image_yuv422;
	*width = mlt_properties_get_int( props, "width" );
	*height = mlt_properties_get_int( props, "height" );
	*buffer = image;
	mlt_frame_set_image( frame, image, size, NULL );
	return 0;
}

static int get_audio( mlt_frame frame, void **buffer, mlt_audio_format *format, int *frequency, int *channels, int *samples )
{
	mlt_properties props = MLT_FRAME_PROPERTIES( frame );
	int size = 0;
	int16_t *audio = mlt_properties_get_data( props, "pcingest.audio", &size );

	if ( !audio )
		return 1;
	*format = mlt_audio_s16;
	*frequency = mlt_properties_get_int( props, "pcingest.frequency" );
	*channels = mlt_properties_get_int( props, "pcingest.channels" );
	*samples = mlt_properties_get_int( props, "pcingest.samples" );
	*buffer = audio;
	mlt_frame_set_audio( frame, audio, *format, size, NULL );
	return 0;
}

static int get_frame( mlt_producer producer, mlt_frame_ptr pframe, int index )
{
	producer_pcingest_t *self = producer->child;
	mlt_properties pprops = MLT_PRODUCER_PROPERTIES( producer );
	mlt_profile profile = mlt_service_profile( MLT_PRODUCER_SERVICE( producer ) );
	mlt_position position = mlt_producer_position( producer );
	double fps = mlt_profile_fps( profile );
	int width = profile->width, height = profile->height;
	size_t image_size = (size_t) width * height * 2;
	int channels = 2, frequency = 48000, progressive = profile->progressive, tff = 1;
	int samples = mlt_sample_calculator( fps, frequency, position );
	int state = STATE_BLACK, flags = PCINGEST_FRAME_BLACK;
	uint8_t *image = mlt_pool_alloc( image_size );
	int16_t *audio;
	mlt_frame frame;

	pthread_mutex_lock( &self->lock );

	if ( !self->ring && now_us() >= self->next_open_us )
	{
		self->next_open_us = now_us() + PCINGEST_REOPEN_US;
		open_ring( self, profile );
	}
	if ( self->ring )
	{
		channels = self->ring->audio_channels;
		progressive = self->ring->progressive;
		tff = self->ring->top_field_first;
	}
	audio = mlt_pool_alloc( PCINGEST_MAX_AUDIO_SAMPLES * PCINGEST_MAX_CHANNELS * 2 );
	memset( audio, 0, (size_t) samples * channels * 2 );

	if ( self->ring && ring_alive( self ) && self->ring->stride == (uint32_t) width * 2 )
	{
		pcingest_header *h = self->ring;
		uint64_t latest = h->next - 1;
		uint64_t target = latest > PCINGEST_LATENCY ? latest - PCINGEST_LATENCY : 0;
		int tries;

		/* (re)start, fallen out of the ring, or the gateway restarted */
		if ( !self->have_cursor || self->cursor + h->slots - 1 < latest || self->cursor > latest + 1 )
		{
			self->cursor = target;
			self->have_cursor = 1;
		}
		if ( self->cursor <= latest )
		{
			for ( tries = 0; tries < 3; tries++ )
			{
				int n = 0, f = read_slot( self, self->cursor, image, audio, &n );
				if ( f >= 0 )
				{
					flags = f;
					samples = n;
					break;
				}
				/* overwritten under us: we're too slow, jump ahead */
				self->cursor = h->next - 1 > PCINGEST_LATENCY ? h->next - 1 - PCINGEST_LATENCY : 0;
			}
			if ( tries < 3 )
			{
				self->cursor++;
				self->delivered++;
				state = flags & PCINGEST_FRAME_BLACK ? STATE_BLACK : STATE_LIVE;
				if ( latest >= self->cursor + PCINGEST_LATENCY + 2 )
				{
					/* the gateway's clock runs faster: drop one */
					self->cursor++;
					self->skips++;
				}
				if ( !self->last_image || self->image_size != image_size )
				{
					free( self->last_image );
					self->last_image = malloc( image_size );
					self->image_size = image_size;
				}
				if ( self->last_image )
					memcpy( self->last_image, image, image_size );
			}
		}
		else if ( self->last_image && self->image_size == image_size )
		{
			/* nothing new yet: repeat, with silence */
			memcpy( image, self->last_image, image_size );
			self->repeats++;
			state = STATE_LIVE;
			flags = PCINGEST_FRAME_HELD;
		}
		else
		{
			fill_black( image, image_size );
		}
	}
	else
	{
		if ( self->ring && !ring_alive( self ) )
			close_ring( self );
		fill_black( image, image_size );
		self->blacks++;
	}

	if ( state != self->last_state )
	{
		mlt_log_info( MLT_PRODUCER_SERVICE( producer ), "pcingest:%s: %s\n", self->name,
			state == STATE_LIVE ? "receiving" : self->ring ? "no signal from the source" : "no gateway" );
		self->last_state = state;
	}
	mlt_properties_set_int64( pprops, "pcingest.delivered", self->delivered );
	mlt_properties_set_int64( pprops, "pcingest.repeats", self->repeats );
	mlt_properties_set_int64( pprops, "pcingest.skips", self->skips );
	mlt_properties_set_int64( pprops, "pcingest.blacks", self->blacks );

	pthread_mutex_unlock( &self->lock );

	*pframe = frame = mlt_frame_init( MLT_PRODUCER_SERVICE( producer ) );
	if ( frame )
	{
		mlt_properties props = MLT_FRAME_PROPERTIES( frame );
		mlt_properties_set_data( props, "pcingest.image", image, image_size, mlt_pool_release, NULL );
		mlt_properties_set_data( props, "pcingest.audio", audio,
			samples * channels * 2, mlt_pool_release, NULL );
		mlt_properties_set_int( props, "pcingest.frequency", frequency );
		mlt_properties_set_int( props, "pcingest.channels", channels );
		mlt_properties_set_int( props, "pcingest.samples", samples );
		mlt_properties_set_int( props, "pcingest.flags", flags );
		mlt_properties_set_int( props, "width", width );
		mlt_properties_set_int( props, "height", height );
		mlt_properties_set_int( props, "format", mlt_image_yuv422 );
		mlt_properties_set_int( props, "progressive", progressive );
		mlt_properties_set_int( props, "top_field_first", tff );
		mlt_properties_set_double( props, "aspect_ratio", mlt_profile_sar( profile ) );
		mlt_frame_push_get_image( frame, get_image );
		mlt_frame_push_audio( frame, (void *) get_audio );
		mlt_frame_set_position( frame, position );
	}
	else
	{
		mlt_pool_release( image );
		mlt_pool_release( audio );
	}

	mlt_producer_prepare_next( producer );
	return 0;
}

static void producer_close( mlt_producer producer )
{
	producer_pcingest_t *self = producer->child;
	close_ring( self );
	free( self->last_image );
	pthread_mutex_destroy( &self->lock );
	producer->close = NULL;
	mlt_producer_close( producer );
	free( self );
}

mlt_producer producer_pcingest_init( mlt_profile profile, mlt_service_type type, const char *id, char *arg )
{
	producer_pcingest_t *self;
	mlt_producer producer;
	mlt_properties props;
	char resource[PCINGEST_NAME_MAX + 16];
	const char *name = arg ? arg : "";
	const char *e = getenv( "MLT_DEFAULT_LIVE_SOURCE_LENGTH" );
	int length = e ? atoi( e ) : 2589411; /* ~24 h at 29.97, like decklink and ndi */
	size_t i;

	/* the shm name: letters, digits, '-' and '_' only */
	while ( *name == '/' )
		name++;
	if ( !*name || strlen( name ) > PCINGEST_NAME_MAX )
		return NULL;
	for ( i = 0; name[i]; i++ )
		if ( !( ( name[i] >= 'a' && name[i] <= 'z' ) || ( name[i] >= 'A' && name[i] <= 'Z' )
			|| ( name[i] >= '0' && name[i] <= '9' ) || name[i] == '-' || name[i] == '_' ) )
			return NULL;

	self = calloc( 1, sizeof( *self ) );
	producer = calloc( 1, sizeof( *producer ) );
	if ( !self || !producer || mlt_producer_init( producer, self ) )
	{
		free( self );
		free( producer );
		return NULL;
	}
	self->parent = producer;
	self->fd = -1;
	strcpy( self->name, name );
	pthread_mutex_init( &self->lock, NULL );

	producer->get_frame = get_frame;
	producer->close = (mlt_destructor) producer_close;

	props = MLT_PRODUCER_PROPERTIES( producer );
	snprintf( resource, sizeof( resource ), "pcingest:%s", name );
	mlt_properties_set( props, "resource", resource );
	mlt_properties_set_int( props, "length", length );
	mlt_properties_set_int( props, "out", length - 1 );
	mlt_properties_set( props, "eof", "loop" );
	mlt_properties_set_int( props, "seekable", 0 );
	mlt_properties_set_int( props, "meta.media.width", profile ? profile->width : 0 );
	mlt_properties_set_int( props, "meta.media.height", profile ? profile->height : 0 );

	/* open now if it's already there (it's retried every second anyway) */
	if ( profile )
		open_ring( self, profile );
	return producer;
}

/*
 * pcingest.h -- Playcast live ingest: shared-memory ring between
 * playcast-live-gateway (writer) and the pcingest producer (reader).
 *
 * The gateway receives a network source (SRT, RTMP, ...), decodes it and
 * converts it to the channel's profile (size, frame rate, field order,
 * 48 kHz audio) on its own clock. It writes exactly one slot per output
 * frame period, always: while the source is missing it keeps writing the
 * last image (for a short hold) and then black, with silence. So the
 * producer never waits for the network: it takes the newest frames and
 * melted's output can't freeze because of the source.
 *
 * Layout of the POSIX shared memory object (/dev/shm/<name>):
 *
 *   pcingest_header
 *   `slots` x slot: pcingest_slot, then the image (YUYV 4:2:2 8-bit,
 *   mlt_image_yuv422, stride * height bytes), then the audio
 *   (PCINGEST_MAX_AUDIO_SAMPLES * audio_channels int16, interleaved)
 *
 * Slots are indexed by the gateway's output frame number: slot = frame %
 * slots. A slot's `frame` is 0 while it's being written and frame + 1 once
 * complete; a reader copies the slot and checks `frame` again (seqlock).
 */

#ifndef PLAYCAST_PCINGEST_H
#define PLAYCAST_PCINGEST_H

#include <stdint.h>

#define PCINGEST_MAGIC 0x50434947u /* "PCIG" */
#define PCINGEST_VERSION 1
#define PCINGEST_SLOTS 8
#define PCINGEST_MAX_AUDIO_SAMPLES 2048 /* 1602 per frame at 29.97 */
#define PCINGEST_MAX_CHANNELS 16
#define PCINGEST_NAME_MAX 64

/* pcingest_header.state */
#define PCINGEST_STATE_SIGNAL 1u   /* the source is connected and decoding */

/* pcingest_slot.flags */
#define PCINGEST_FRAME_HELD 1u     /* no new image from the source: repeated */
#define PCINGEST_FRAME_BLACK 2u    /* no source: black and silence */

typedef struct
{
	uint32_t magic;
	uint32_t version;
	uint32_t header_size;
	uint32_t width;
	uint32_t height;
	uint32_t stride;            /* bytes per image line (width * 2) */
	uint32_t fps_num;
	uint32_t fps_den;
	int32_t progressive;
	int32_t top_field_first;
	uint32_t audio_rate;        /* 48000 */
	uint32_t audio_channels;    /* int16 interleaved */
	uint32_t slots;
	uint32_t slot_size;         /* bytes, including pcingest_slot */
	uint64_t slots_offset;      /* from the start of the mapping */
	volatile int32_t writer_pid;
	volatile uint32_t state;    /* PCINGEST_STATE_* */
	volatile uint64_t next;     /* last committed frame + 1 */
	volatile uint64_t heartbeat_us; /* CLOCK_MONOTONIC of the last write */
	char input[256];            /* what's coming in, for logs and status */
} pcingest_header;

typedef struct
{
	volatile uint64_t frame;    /* frame + 1 when complete, 0 while writing */
	uint32_t flags;             /* PCINGEST_FRAME_* */
	uint32_t samples;           /* audio samples in this slot */
} pcingest_slot;

static inline pcingest_slot *pcingest_slot_at( pcingest_header *h, uint64_t frame )
{
	return (pcingest_slot *) ( (uint8_t *) h + h->slots_offset
		+ (uint64_t) ( frame % h->slots ) * h->slot_size );
}

static inline uint8_t *pcingest_image( pcingest_slot *slot )
{
	return (uint8_t *) slot + sizeof( pcingest_slot );
}

static inline int16_t *pcingest_audio( pcingest_header *h, pcingest_slot *slot )
{
	return (int16_t *) ( pcingest_image( slot ) + (uint64_t) h->stride * h->height );
}

static inline uint32_t pcingest_slot_size( uint32_t stride, uint32_t height, uint32_t channels )
{
	uint64_t size = sizeof( pcingest_slot ) + (uint64_t) stride * height
		+ (uint64_t) PCINGEST_MAX_AUDIO_SAMPLES * channels * 2;
	return (uint32_t) ( ( size + 63 ) & ~63ull );
}

#endif

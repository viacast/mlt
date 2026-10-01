/*
 * record_tap.h -- Playcast recording tap: shared-memory ring of the frames
 * and audio the decklink consumer sends to the card.
 *
 * Layout of the POSIX shared memory object (/dev/shm/<name>):
 *
 *   pctap_header
 *   video ring: `slots` x (pctap_video_slot + stride*height bytes, UYVY 8-bit)
 *   audio ring: `slots` x (pctap_audio_slot + PCTAP_MAX_AUDIO_SAMPLES*channels
 *               int16 samples, interleaved)
 *
 * Both rings are indexed by the consumer's frame number (the one used to
 * schedule the frame on the card): slot = frame % slots. Video and audio are
 * written by different threads, so a reader pairs them by frame number.
 *
 * The writer never waits for a reader: a slot's `frame` field is set to 0
 * while it is being written and to frame+1 once complete, so a reader copies
 * the payload and then checks `frame` again (seqlock) to detect overwrites.
 * A slow reader loses frames; the SDI output is never held back.
 */

#ifndef PLAYCAST_RECORD_TAP_H
#define PLAYCAST_RECORD_TAP_H

#include <stdint.h>

#define PCTAP_MAGIC 0x50435441u /* "PCTA" */
#define PCTAP_VERSION 1
#define PCTAP_SLOTS 32
#define PCTAP_MAX_AUDIO_SAMPLES 2048 /* 1602 per frame at 29.97 */
#define PCTAP_MAX_CHANNELS 16

/* pctap_video_slot.flags */
#define PCTAP_FLAG_NO_AUDIO 1u /* not playing at normal speed: the card gets no
                                  audio for this frame, so silence is expected */

typedef struct
{
	uint32_t magic;
	uint32_t version;
	uint32_t header_size;
	uint32_t width;
	uint32_t height;
	uint32_t stride;           /* bytes per video line (width * 2 for UYVY) */
	uint32_t fps_num;
	uint32_t fps_den;
	int32_t progressive;
	int32_t top_field_first;
	uint32_t audio_rate;
	uint32_t audio_channels;   /* int16 interleaved */
	uint32_t slots;
	uint32_t video_slot_size;  /* bytes, including pctap_video_slot */
	uint32_t audio_slot_size;  /* bytes, including pctap_audio_slot */
	uint32_t reserved;
	uint64_t video_offset;     /* from the start of the mapping */
	uint64_t audio_offset;
	volatile int32_t active;   /* 0 once the writer has detached */
	int32_t writer_pid;
	volatile uint64_t video_next;   /* last committed video frame + 1 */
	volatile uint64_t audio_next;   /* last committed audio frame + 1 */
	volatile uint64_t frames_late;  /* DeckLink completion results */
	volatile uint64_t frames_dropped;
	volatile uint64_t heartbeat_us; /* CLOCK_MONOTONIC of the last write */
} pctap_header;

typedef struct
{
	volatile uint64_t frame; /* frame + 1 when complete, 0 while writing */
	uint32_t repeat;         /* 1: the card repeated the previous image */
	uint32_t flags;          /* PCTAP_FLAG_* */
} pctap_video_slot;

typedef struct
{
	volatile uint64_t frame; /* frame + 1 when complete, 0 while writing */
	uint32_t samples;
	uint32_t channels;
} pctap_audio_slot;

static inline pctap_video_slot *pctap_video(pctap_header *h, uint64_t frame)
{
	return (pctap_video_slot *) ((uint8_t *) h + h->video_offset
		+ (uint64_t) (frame % h->slots) * h->video_slot_size);
}

static inline pctap_audio_slot *pctap_audio(pctap_header *h, uint64_t frame)
{
	return (pctap_audio_slot *) ((uint8_t *) h + h->audio_offset
		+ (uint64_t) (frame % h->slots) * h->audio_slot_size);
}

#ifdef __cplusplus
extern "C" {
#endif

/* Writer side (decklink consumer) */
typedef struct pctap_writer pctap_writer;

pctap_writer *pctap_open(const char *name, int width, int height, int fps_num, int fps_den,
	int progressive, int top_field_first, int audio_rate, int audio_channels);
void pctap_write_video(pctap_writer *tap, uint64_t frame, const uint8_t *image, int repeat, uint32_t flags);
void pctap_write_audio(pctap_writer *tap, uint64_t frame, const int16_t *pcm, int samples, int channels);
void pctap_set_counters(pctap_writer *tap, uint64_t late, uint64_t dropped);
const char *pctap_name(pctap_writer *tap);
void pctap_close(pctap_writer *tap);

#ifdef __cplusplus
}
#endif

#endif

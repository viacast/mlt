/*
 * consumer_decklink.cpp -- output through Blackmagic Design DeckLink
 * Copyright (C) 2010-2018 Meltytech, LLC
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
 * License along with consumer library; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA  02110-1301  USA
 */

#define __STDC_FORMAT_MACROS  /* see inttypes.h */
#include <atomic>
#include <framework/mlt.h>
#include <stdlib.h>
#include <string.h>
#include <pthread.h>
#include <unistd.h>
#include <sys/time.h>
#include <limits.h>
#include <pthread.h>
#include "common.h"

#include <DeckLinkAPI.h>

extern "C" {
#include "libavcodec/avcodec.h"
#include "libavcodec/packet.h"
#include "libavformat/avformat.h"
#include "libavutil/avutil.h"
#include "libavutil/frame.h"
#include "libavdevice/avdevice.h"
}

#include "libklvanc/vanc.h"
#include "libklvanc/vanc-lines.h"
#include "libklvanc/pixels.h"

#define SWAB_SLICED_ALIGN_POW 5
static int swab_sliced( int id, int idx, int jobs, void* cookie )
{
	unsigned char** args = (unsigned char**)cookie;
	ssize_t sz = (ssize_t)args[2];
	ssize_t bsz = ( ( sz / jobs + ( 1 << SWAB_SLICED_ALIGN_POW ) - 1 ) >> SWAB_SLICED_ALIGN_POW ) << SWAB_SLICED_ALIGN_POW;
	ssize_t offset = bsz * idx;

	if ( offset < sz )
	{
		if ( ( offset + bsz ) > sz )
			bsz = sz - offset;

		swab2( args[0] + offset, args[1] + offset, bsz );
	}

	return 0;
};

#define SCTE104_VANC_LINE 9

class DeckLinkVideoFrame : public IDeckLinkMutableVideoFrame
{
public:
    virtual HRESULT STDMETHODCALLTYPE GetAncillaryData(IDeckLinkVideoFrameAncillary **ancillary)
    {
        *ancillary = _ancillary;
        if (_ancillary) {
            _ancillary->AddRef();
            return S_OK;
        } else {
            return S_FALSE;
        }
    }
    virtual HRESULT STDMETHODCALLTYPE SetAncillaryData(IDeckLinkVideoFrameAncillary* ancillary) {
        if (_ancillary)
            _ancillary->Release();
        _ancillary = ancillary;
        _ancillary->AddRef();
        return S_OK;
		}
    IDeckLinkVideoFrameAncillary *_ancillary;
};

struct SCTE104
{
	unsigned int splice_insert_type;
	unsigned int splice_event_id;
	unsigned short unique_program_id;
	unsigned short pre_roll_time;
	unsigned short brk_duration;
	unsigned char avail_num;
	unsigned char avails_expected;
	unsigned char auto_return_flag;
};

static const unsigned PREROLL_MINIMUM = 3;

enum
{
	OP_NONE = 0,
	OP_OPEN,
	OP_START,
	OP_STOP,
	OP_EXIT
};

class DeckLinkConsumer
	: public IDeckLinkVideoOutputCallback
	, public IDeckLinkAudioOutputCallback
{
private:
	mlt_consumer_s              m_consumer;
	IDeckLink*                  m_deckLink;
	IDeckLinkOutput*            m_deckLinkOutput;
	IDeckLinkDisplayMode*       m_displayMode;
	int                         m_width;
	int                         m_height;
	BMDTimeValue                m_duration;
	BMDTimeScale                m_timescale;
	double                      m_fps;
	uint64_t                    m_count;
	int                         m_outChannels;
	int                         m_inChannels;
	bool                        m_isAudio;
	int                         m_isKeyer;
	IDeckLinkKeyer*             m_deckLinkKeyer;
	bool                        m_terminate_on_pause;
	uint32_t                    m_preroll;
	uint32_t                    m_reprio;

	mlt_deque                   m_aqueue;
	pthread_mutex_t             m_aqueue_lock;
	mlt_deque                   m_frames;
	mlt_deque                   m_frames_interim;

	pthread_mutex_t             m_op_lock;
	pthread_mutex_t             m_op_arg_mutex;
	pthread_cond_t              m_op_arg_cond;
	int                         m_op_id;
	int                         m_op_res;
	int                         m_op_arg;
	pthread_t                   m_op_thread;
	bool                        m_sliced_swab;
	uint8_t*                    m_buffer;

	bool                        m_supports_vanc;
	struct klvanc_context_s*    m_vanc_ctx;
	IDeckLinkVideoConversion*   m_decklinkVideoConversion;
	uint16_t 										m_cdp_sequence_num;

	IDeckLinkDisplayMode* getDisplayMode()
	{
		mlt_profile profile = mlt_service_profile( MLT_CONSUMER_SERVICE( getConsumer() ) );
		IDeckLinkDisplayModeIterator* iter = NULL;
		IDeckLinkDisplayMode* mode = NULL;
		IDeckLinkDisplayMode* result = 0;

		if ( m_deckLinkOutput->GetDisplayModeIterator( &iter ) == S_OK )
		{
			while ( !result && iter->Next( &mode ) == S_OK )
			{
				m_width = mode->GetWidth();
				m_height = mode->GetHeight();
				mode->GetFrameRate( &m_duration, &m_timescale );
				m_fps = (double) m_timescale / m_duration;
				int p = mode->GetFieldDominance() == bmdProgressiveFrame;
				mlt_log_verbose( getConsumer(), "BMD mode %dx%d %.3f fps prog %d\n", m_width, m_height, m_fps, p );

				if ( m_width == profile->width && p == profile->progressive
					 && (int) m_fps == (int) mlt_profile_fps( profile )
					 && ( m_height == profile->height || ( m_height == 486 && profile->height == 480 ) ) )
					result = mode;
				else
					SAFE_RELEASE( mode );
			}
			SAFE_RELEASE( iter );
		}

		return result;
	}

public:
	mlt_consumer getConsumer()
		{ return &m_consumer; }

	DeckLinkConsumer()
	{
		pthread_mutexattr_t mta;

		m_displayMode = NULL;
		m_deckLinkKeyer = NULL;
		m_deckLinkOutput = NULL;
		m_deckLink = NULL;
		m_aqueue = mlt_deque_init();
		m_frames = mlt_deque_init();
		m_frames_interim = mlt_deque_init();
		m_buffer = NULL;

		// operation locks
		m_op_id = OP_NONE;
		m_op_arg = 0;
		pthread_mutexattr_init( &mta );
		pthread_mutexattr_settype( &mta, PTHREAD_MUTEX_RECURSIVE );
		pthread_mutex_init( &m_op_lock, &mta );
		pthread_mutex_init( &m_op_arg_mutex, &mta );
		pthread_mutex_init( &m_aqueue_lock, &mta );
		pthread_mutexattr_destroy( &mta );
		pthread_cond_init( &m_op_arg_cond, NULL );
		pthread_create( &m_op_thread, NULL, op_main, this );
	}

	virtual ~DeckLinkConsumer()
	{
		mlt_log_debug( getConsumer(), "%s: entering\n",  __FUNCTION__ );

		SAFE_RELEASE( m_displayMode );
		SAFE_RELEASE( m_deckLinkKeyer );
		SAFE_RELEASE( m_deckLinkOutput );
		SAFE_RELEASE( m_deckLink );

		mlt_deque_close( m_aqueue );
		mlt_deque_close( m_frames );
		mlt_deque_close( m_frames_interim );

		op(OP_EXIT, 0);
		mlt_log_debug( getConsumer(), "%s: waiting for op thread\n", __FUNCTION__ );
		pthread_join(m_op_thread, NULL);
		mlt_log_debug( getConsumer(), "%s: finished op thread\n", __FUNCTION__ );

		pthread_mutex_destroy( &m_aqueue_lock );
		pthread_mutex_destroy(&m_op_lock);
		pthread_mutex_destroy(&m_op_arg_mutex);
		pthread_cond_destroy(&m_op_arg_cond);

		mlt_log_debug( getConsumer(), "%s: exiting\n", __FUNCTION__ );
	}

	int op(int op_id, int arg)
	{
		int r;

		// lock operation mutex
		pthread_mutex_lock(&m_op_lock);

		mlt_log_debug( getConsumer(), "%s: op_id=%d\n", __FUNCTION__, op_id );

		// notify op id
		pthread_mutex_lock(&m_op_arg_mutex);
		m_op_id = op_id;
		m_op_arg = arg;
		pthread_cond_signal(&m_op_arg_cond);
		pthread_mutex_unlock(&m_op_arg_mutex);

		// wait op done
		pthread_mutex_lock(&m_op_arg_mutex);
		while(OP_NONE != m_op_id)
			pthread_cond_wait(&m_op_arg_cond, &m_op_arg_mutex);
		pthread_mutex_unlock(&m_op_arg_mutex);

		// save result
		r = m_op_res;

		mlt_log_debug( getConsumer(), "%s: r=%d\n", __FUNCTION__, r );

		// unlock operation mutex
		pthread_mutex_unlock(&m_op_lock);

		return r;
	}

protected:

	static void* op_main(void* thisptr)
	{
		DeckLinkConsumer* d = static_cast<DeckLinkConsumer*>(thisptr);

		mlt_log_debug( d->getConsumer(), "%s: entering\n", __FUNCTION__ );

		for (;;)
		{
			int o, r = 0;

			// wait op command
			pthread_mutex_lock ( &d->m_op_arg_mutex );
			while ( OP_NONE == d->m_op_id )
				pthread_cond_wait( &d->m_op_arg_cond, &d->m_op_arg_mutex );
			pthread_mutex_unlock( &d->m_op_arg_mutex );
			o = d->m_op_id;

			mlt_log_debug( d->getConsumer(), "%s:%d d->m_op_id=%d\n", __FUNCTION__, __LINE__, d->m_op_id );

			switch ( d->m_op_id )
			{
				case OP_OPEN:
					r = d->m_op_res = d->open( d->m_op_arg );
					break;

				case OP_START:
					r = d->m_op_res = d->start( d->m_op_arg );
					break;

				case OP_STOP:
					r = d->m_op_res = d->stop();
					break;
			};

			// notify op done
			pthread_mutex_lock( &d->m_op_arg_mutex );
			d->m_op_id = OP_NONE;
			pthread_cond_signal( &d->m_op_arg_cond );
			pthread_mutex_unlock( &d->m_op_arg_mutex );

			// post for async
			if ( OP_START == o && r )
				d->preroll();

			if ( OP_EXIT == o )
			{
				mlt_log_debug( d->getConsumer(), "%s: exiting\n", __FUNCTION__ );
				return NULL;
			}
		};

		return NULL;
	}

	bool open( unsigned card = 0 )
	{
		unsigned i = 0;
#ifdef _WIN32
		IDeckLinkIterator* deckLinkIterator = NULL;
		HRESULT result =  CoInitialize( NULL );
		if ( FAILED( result ) )
		{
			mlt_log_error( getConsumer(), "COM initialization failed\n" );
			return false;
		}
		result = CoCreateInstance( CLSID_CDeckLinkIterator, NULL, CLSCTX_ALL, IID_IDeckLinkIterator, (void**) &deckLinkIterator );
		if ( FAILED( result ) )
		{
			mlt_log_warning( getConsumer(), "The DeckLink drivers not installed.\n" );
			return false;
		}
#else
		IDeckLinkIterator* deckLinkIterator = CreateDeckLinkIteratorInstance();

		if ( !deckLinkIterator )
		{
			mlt_log_warning( getConsumer(), "The DeckLink drivers not installed.\n" );
			return false;
		}
#endif

		// Connect to the Nth DeckLink instance
		for ( i = 0; deckLinkIterator->Next( &m_deckLink ) == S_OK ; i++)
		{
			if( i == card )
				break;
			else
				SAFE_RELEASE( m_deckLink );
		}
		SAFE_RELEASE( deckLinkIterator );
		if ( !m_deckLink )
		{
			mlt_log_error( getConsumer(), "DeckLink card not found\n" );
			return false;
		}

		// Obtain the audio/video output interface (IDeckLinkOutput)
		if ( m_deckLink->QueryInterface( IID_IDeckLinkOutput, (void**)&m_deckLinkOutput ) != S_OK )
		{
			mlt_log_error( getConsumer(), "No DeckLink cards support output\n" );
			SAFE_RELEASE( m_deckLink );
			return false;
		}

		// Get the keyer interface
		IDeckLinkAttributes *deckLinkAttributes = 0;
		if ( m_deckLink->QueryInterface( IID_IDeckLinkAttributes, (void**) &deckLinkAttributes ) == S_OK )
		{
#ifdef _WIN32
			BOOL flag = FALSE;
#else
			bool flag = false;
#endif
			if ( deckLinkAttributes->GetFlag( BMDDeckLinkSupportsInternalKeying, &flag ) == S_OK && flag )
			{
				if ( m_deckLink->QueryInterface( IID_IDeckLinkKeyer, (void**) &m_deckLinkKeyer ) != S_OK )
				{
					mlt_log_error( getConsumer(), "Failed to get keyer\n" );
					SAFE_RELEASE( m_deckLinkOutput );
					SAFE_RELEASE( m_deckLink );
					return false;
				}
			}
			SAFE_RELEASE( deckLinkAttributes );
		}

		if (!m_vanc_ctx && klvanc_context_create(&m_vanc_ctx) < 0) {
        mlt_log_error(getConsumer(), "Cannot create VANC library context\n");
        return AVERROR(ENOMEM);
    }
		m_supports_vanc = true;

		// Provide this class as a delegate to the audio and video output interfaces
		m_deckLinkOutput->SetScheduledFrameCompletionCallback( this );
		m_deckLinkOutput->SetAudioCallback( this );

		return true;
	}

	int preroll()
	{
		mlt_properties properties = MLT_CONSUMER_PROPERTIES( getConsumer() );

		mlt_log_debug( getConsumer(), "%s: starting\n", __FUNCTION__ );

		if ( !mlt_properties_get_int( properties, "running" ) )
			return 0;

		mlt_log_verbose( getConsumer(), "preroll %u frames\n", m_preroll );

		// preroll frames
		for ( unsigned i = 0; i < m_preroll ; i++ )
			ScheduleNextFrame( true );

		// start audio preroll
		if ( m_isAudio )
			m_deckLinkOutput->BeginAudioPreroll( );
		else
			m_deckLinkOutput->StartScheduledPlayback( 0, m_timescale, 1.0 );

		mlt_log_debug( getConsumer(), "%s: exiting\n", __FUNCTION__ );

		return 0;
	}

	bool start( unsigned preroll )
	{
		mlt_properties properties = MLT_CONSUMER_PROPERTIES( getConsumer() );

		// Initialize members
		m_count = 0;
		m_buffer = NULL;
		preroll = preroll < PREROLL_MINIMUM ? PREROLL_MINIMUM : preroll;
		m_inChannels = mlt_properties_get_int( properties, "channels" );
		if( m_inChannels <= 2 )
		{
			m_outChannels = 2;
		}
		else if( m_inChannels <= 8 )
		{
			m_outChannels = 8;
		}
		else
		{
			m_outChannels = 16;
		}
		m_isAudio = !mlt_properties_get_int( properties, "audio_off" );
		m_terminate_on_pause = mlt_properties_get_int( properties, "terminate_on_pause" );

		m_displayMode = getDisplayMode();
		if ( !m_displayMode )
		{
			mlt_log_error( getConsumer(), "Profile is not compatible with decklink.\n" );
			return false;
		}
		mlt_properties_set_int( properties, "top_field_first", m_displayMode->GetFieldDominance() == bmdUpperFieldFirst );

		// Set the keyer
		if ( m_deckLinkKeyer && ( m_isKeyer = mlt_properties_get_int( properties, "keyer" ) ) )
		{
			bool external = ( m_isKeyer == 2 );
			double level = mlt_properties_get_double( properties, "keyer_level" );

			if ( m_deckLinkKeyer->Enable( external ) != S_OK )
				mlt_log_error( getConsumer(), "Failed to enable %s keyer\n",
					external ? "external" : "internal" );
			m_deckLinkKeyer->SetLevel( level <= 1 ? ( level > 0 ? 255 * level : 255 ) : 255 );
		}
		else if ( m_deckLinkKeyer )
		{
			m_deckLinkKeyer->Disable();
		}

		BMDVideoOutputFlags flags = bmdVideoOutputRP188 | bmdVideoOutputVITC;
		if (m_supports_vanc)
			flags |= bmdVideoOutputVANC;
		// Set the video output mode
		if ( S_OK != m_deckLinkOutput->EnableVideoOutput( m_displayMode->GetDisplayMode(), flags ) )
		{
			mlt_log_error( getConsumer(), "Failed to enable video output\n" );
			return false;
		}

		// Set the audio output mode
		if ( m_isAudio && S_OK != m_deckLinkOutput->EnableAudioOutput( bmdAudioSampleRate48kHz, bmdAudioSampleType16bitInteger,
			m_outChannels, bmdAudioOutputStreamTimestamped ) )
		{
			mlt_log_error( getConsumer(), "Failed to enable audio output\n" );
			stop();
			return false;
		}

		m_preroll = preroll;
		m_reprio = 2;

		for ( unsigned i = 0; i < ( m_preroll + 2 ) ; i++)
		{
			IDeckLinkMutableVideoFrame* frame;

			// Generate a DeckLink video frame
			if ( S_OK != m_deckLinkOutput->CreateVideoFrame( m_width, m_height,
				m_width * ( m_isKeyer? 4 : 2 ), m_isKeyer? bmdFormat8BitARGB : bmdFormat8BitYUV, bmdFrameFlagDefault, &frame ) )
			{
				mlt_log_error( getConsumer(), "%s: CreateVideoFrame (%d) failed\n", __FUNCTION__, i );
				return false;
			}

			mlt_deque_push_back( m_frames, frame );
		}

		m_decklinkVideoConversion = CreateVideoConversionInstance();

		// Set the running state
		mlt_properties_set_int( properties, "running", 1 );

		return true;
	}

	bool stop()
	{
		mlt_properties properties = MLT_CONSUMER_PROPERTIES( getConsumer() );

		mlt_log_debug( getConsumer(), "%s: starting\n", __FUNCTION__ );

    klvanc_context_destroy(m_vanc_ctx);
		m_vanc_ctx = NULL;

		// Stop the audio and video output streams immediately
		if ( m_deckLinkOutput )
		{
			m_deckLinkOutput->StopScheduledPlayback( 0, 0, 0 );
			m_deckLinkOutput->DisableAudioOutput();
			m_deckLinkOutput->DisableVideoOutput();
		}

		pthread_mutex_lock( &m_aqueue_lock );
		while ( mlt_frame frame = (mlt_frame) mlt_deque_pop_back( m_aqueue ) )
			mlt_frame_close( frame );
		pthread_mutex_unlock( &m_aqueue_lock );

		m_buffer = NULL;
		IDeckLinkMutableVideoFrame* frame;
		while ( frame = (IDeckLinkMutableVideoFrame*) mlt_deque_pop_back( m_frames ) )
			SAFE_RELEASE( frame );
		while ( frame = (IDeckLinkMutableVideoFrame*) mlt_deque_pop_back( m_frames_interim ) )
			SAFE_RELEASE( frame );

		SAFE_RELEASE(m_decklinkVideoConversion);

		// set running state is 0
		mlt_properties_set_int( properties, "running", 0 );

		mlt_consumer_stopped( getConsumer() );

		mlt_log_debug( getConsumer(), "%s: exiting\n", __FUNCTION__ );

		return true;
	}

	void renderAudio( mlt_frame frame )
	{
		mlt_properties properties;
		properties = MLT_FRAME_PROPERTIES( frame );
		mlt_properties_set_int64( properties, "m_count", m_count);
		mlt_properties_inc_ref( properties );
		pthread_mutex_lock( &m_aqueue_lock );
		mlt_deque_push_back( m_aqueue, frame );
		mlt_log_debug( getConsumer(), "%s:%d frame=%p, len=%d\n", __FUNCTION__, __LINE__, frame, mlt_deque_count( m_aqueue ));
		pthread_mutex_unlock( &m_aqueue_lock );
	}

	int create_ancillary_data(DeckLinkVideoFrame *decklink_frame, BMDPixelFormat pixel_format) 
	{
		if (!m_supports_vanc)
			return S_FALSE;
		int ret = S_OK;
		IDeckLinkVideoFrameAncillary *vanc;
		if (decklink_frame->GetAncillaryData(&vanc) == S_FALSE) {
			ret = m_deckLinkOutput->CreateAncillaryData(bmdFormat10BitYUV, &vanc);
			if (ret != S_OK) {
					mlt_log_error(getConsumer(), "Failed to create vanc\n");
					goto exit_create_ancillary_data;
			}
			decklink_frame->SetAncillaryData(vanc);
		}
exit_create_ancillary_data:
		if (vanc)
			vanc->Release();
		return ret;
	}

	void insert_vanc(IDeckLinkVideoFrameAncillary *vanc, struct klvanc_line_set_s *vanc_lines)
	{
			int result;
			/* Now that we've got all the VANC lines in a nice orderly manner, generate the
				final VANC sections for the Decklink output */
			for (int i = 0; i < vanc_lines->num_lines; i++) {
					struct klvanc_line_s *line = vanc_lines->lines[i];
					int real_line;
					void *buf;

					if (!line)
							break;

					/* FIXME: include hack for certain Decklink cards which mis-represent
						line numbers for pSF frames */
					real_line = line->line_number;

					result = vanc->GetBufferForVerticalBlankingLine(real_line, &buf);
					if (result != S_OK) {
							mlt_log_error(getConsumer(), "Failed to get VANC line %d: %d\n", real_line, result);
							continue;
					}

					/* Generate the full line taking into account all VANC packets on that line */
					result = klvanc_generate_vanc_line_v210(m_vanc_ctx, line, (uint8_t *) buf, m_width);
					if (result) {
							mlt_log_error(getConsumer(), "Failed to generate VANC line\n");
							continue;
					}
			}
	}

	void construct_cc(mlt_frame frame, struct klvanc_line_set_s *vanc_lines)
	{
			struct klvanc_packet_eia_708b_s *cdp;
			uint16_t *cdp_words;
			uint16_t len;
			uint8_t cc_count;
			int ret, i;

			char *cc_size = mlt_properties_get(MLT_FRAME_PROPERTIES(frame), "meta.cc-size");
			if (!cc_size || (cc_size && !strlen(cc_size)))
				return;

			int size = atoi(cc_size);
			if (!size)
					return;

			const uint8_t *data = (uint8_t *) mlt_properties_get(MLT_FRAME_PROPERTIES(frame), "meta.cc-data");

			if (!data) {
				mlt_log_error(getConsumer(), "error constructing cc. size > 0 but data is null");
				return;
			}

			cc_count = size / 3;

			ret = klvanc_create_eia708_cdp(&cdp);
			if (ret)
					return;

			ret = klvanc_set_framerate_EIA_708B(cdp, m_duration, m_timescale);
			if (ret) {
					mlt_log_error(getConsumer(), "Invalid framerate specified: %lld/%lld\n", m_timescale, m_duration);
					klvanc_destroy_eia708_cdp(cdp);
					return;
			}

			if (cc_count > KLVANC_MAX_CC_COUNT) {
				mlt_log_error(getConsumer(), "Illegal cc_count received: %d\n", cc_count);
				cc_count = KLVANC_MAX_CC_COUNT;
			}

			/* CC data */
			cdp->header.ccdata_present = 1;
			cdp->header.caption_service_active = 1;
			cdp->ccdata.cc_count = cc_count;
			for (i = 0; i < cc_count; i++) {
					if (data [3*i] & 0x04)
							cdp->ccdata.cc[i].cc_valid = 1;
					cdp->ccdata.cc[i].cc_type = data[3*i] & 0x03;
					cdp->ccdata.cc[i].cc_data[0] = data[3*i+1];
					cdp->ccdata.cc[i].cc_data[1] = data[3*i+2];
			}

			klvanc_finalize_EIA_708B(cdp, m_cdp_sequence_num++);
			ret = klvanc_convert_EIA_708B_to_words(cdp, &cdp_words, &len);
			klvanc_destroy_eia708_cdp(cdp);
			if (ret != 0) {
					mlt_log_error(getConsumer(), "Failed converting 708 packet to words\n");
					return;
			}

			ret = klvanc_line_insert(m_vanc_ctx, vanc_lines, cdp_words, len, 11, 0);
			free(cdp_words);
			if (ret != 0) {
					mlt_log_error(getConsumer(), "VANC line insertion failed\n");
					return;
			}
	}

	int construct_vanc_cc(mlt_frame frame, DeckLinkVideoFrame *decklink_frame)
	{
			struct klvanc_line_set_s vanc_lines = { 0 };
			int ret = 0, i, result;

			if (!m_supports_vanc)
					return S_OK;

			construct_cc(frame, &vanc_lines);

			IDeckLinkVideoFrameAncillary *vanc;
			result = decklink_frame->GetAncillaryData(&vanc);

			if (result != S_OK) {
					mlt_log_error(getConsumer(), "Failed to get vanc cc\n");
					ret = AVERROR(EIO);
					goto done;
			}

			insert_vanc(vanc, &vanc_lines);
			vanc->Release();

done:
			for (i = 0; i < vanc_lines.num_lines; i++)
					klvanc_line_free(vanc_lines.lines[i]);

			return ret;
	}
	
	struct SCTE104 parse_scte_104(const char *scte_104_str) {
		struct SCTE104 scte_104;

		if (!scte_104_str)
				return scte_104;

		// increased for every event
		static unsigned int splice_event_id = 0;
		// only changes on melted restart
		static unsigned short unique_program_id = 0x1234;

		scte_104.splice_event_id = ++splice_event_id;
		scte_104.unique_program_id = unique_program_id;

		char scte_cpy[1024];
		strcpy(scte_cpy, scte_104_str);

		char args[32][64];
		int i = 0, j;

		char *token = strtok(scte_cpy, " ");
		while( token != NULL && i < 32 ) {
				strcpy(args[i++], token);
				token = strtok(NULL, " ");
		}

		for (j = 0; j < i; ++j) {
				if (j + 1 >= i)
						continue;
				if (!strcmp("-insert_type", args[j]))
						scte_104.splice_insert_type = atoi(args[j+1]);
				if (!strcmp("-event_id", args[j]))
						scte_104.splice_event_id = atoi(args[j+1]);
				if (!strcmp("-program_id", args[j]))
						scte_104.unique_program_id = atoi(args[j+1]);
				if (!strcmp("-pre_roll", args[j]))
						scte_104.pre_roll_time = atoi(args[j+1]);
				if (!strcmp("-break_duration", args[j]))
						scte_104.brk_duration = atoi(args[j+1]);
				if (!strcmp("-avail_num", args[j]))
						scte_104.avail_num = atoi(args[j+1]);
				if (!strcmp("-avails_expected", args[j]))
						scte_104.avails_expected = atoi(args[j+1]);
				if (!strcmp("-auto_return", args[j]))
						scte_104.auto_return_flag = atoi(args[j+1]);
		}

		return scte_104;
	}

	int construct_scte_104(mlt_frame frame, DeckLinkVideoFrame *decklink_frame)
	{
			struct klvanc_line_set_s vanc_lines = { 0 };
			struct klvanc_packet_scte_104_s *pkt;
			struct klvanc_multiple_operation_message_operation *op;
			int ret = 0, i, result;
			uint16_t *words;
			uint16_t wordCount;

			char *scte = mlt_properties_get(MLT_FRAME_PROPERTIES(frame), "meta.scte-104");
			if (!scte || (scte && !strlen(scte)))
				return S_OK;

			struct SCTE104 scte_104 = parse_scte_104(scte);
			if (!m_supports_vanc || !scte_104.splice_event_id)
					return S_OK;

			IDeckLinkVideoFrameAncillary *vanc;
			result = decklink_frame->GetAncillaryData(&vanc);

			if (result != S_OK) {
					mlt_log_error(getConsumer(), "Failed to get vanc scte104\n");
					return AVERROR(EIO);
			}

			result = klvanc_alloc_SCTE_104(0xffff, &pkt);
			if (result != S_OK) {
					mlt_log_error(getConsumer(), "Failed to alloc scte104\n");
					return AVERROR(EIO);
			}

			result =  klvanc_SCTE_104_Add_MOM_Op(pkt, MO_SPLICE_REQUEST_DATA, &op);
			if (result != S_OK) {
					mlt_log_error(getConsumer(), "Failed to add SCTE 104 op\n");
					ret = AVERROR(EIO);
					goto scte104_done;
			}

			op->sr_data = *(klvanc_splice_request_data *) &scte_104;

			result = klvanc_dump_SCTE_104(m_vanc_ctx, pkt);
			if (result != S_OK) {
					mlt_log_error(getConsumer(), "Failed to dump SCTE 104 packet\n");
					ret = AVERROR(EIO);
					goto scte104_done;
			}

			result = klvanc_convert_SCTE_104_to_words(m_vanc_ctx, pkt, &words, &wordCount);
			if (result != S_OK)  {
					mlt_log_error(getConsumer(), "Failed to dump SCTE 104 packet\n");
					ret = AVERROR(EIO);
					goto scte104_done;
			}

			ret = klvanc_line_insert(m_vanc_ctx, &vanc_lines, words, wordCount, SCTE104_VANC_LINE, 0);
			if (ret != 0) {
					mlt_log_error(getConsumer(), "VANC line insertion failed\n");
					ret = AVERROR(EIO);
					goto scte104_done;
			}

			insert_vanc(vanc, &vanc_lines);
			vanc->Release();

scte104_done:
			for (i = 0; i < vanc_lines.num_lines; i++)
					klvanc_line_free(vanc_lines.lines[i]);
			if (pkt)
					klvanc_free_SCTE_104(pkt);
			return ret;
	}

	void renderVideo( mlt_frame frame )
	{
		HRESULT hr;
		mlt_image_format format = m_isKeyer? mlt_image_rgb24a : mlt_image_yuv422;
		uint8_t* image = 0;
		int rendered = mlt_properties_get_int( MLT_FRAME_PROPERTIES(frame), "rendered");
		mlt_properties consumer_properties = MLT_CONSUMER_PROPERTIES( getConsumer() );
		int stride = m_width * ( m_isKeyer? 4 : 2 );
		int height = m_height;
		DeckLinkVideoFrame* decklinkFrame =
			static_cast<DeckLinkVideoFrame*>( mlt_deque_pop_front( m_frames ) );

		mlt_log_debug( getConsumer(), "%s: entering\n", __FUNCTION__ );

		m_sliced_swab = mlt_properties_get_int( consumer_properties, "sliced_swab" );

		if ( rendered && !mlt_frame_get_image( frame, &image, &format, &m_width, &height, 0 ) )
		{
			if ( decklinkFrame )
				decklinkFrame->GetBytes( (void**) &m_buffer );

			if ( m_buffer )
			{
				// NTSC SDI is always 486 lines
				if ( m_height == 486 && height == 480 )
				{
					// blank first 6 lines
					if ( m_isKeyer )
					{
						memset( m_buffer, 0, stride * 6 );
						m_buffer += stride * 6;
					}
					else for ( int i = 0; i < m_width * 6; i++ )
					{
						*m_buffer++ = 128;
						*m_buffer++ = 16;
					}
				}
				if ( !m_isKeyer )
				{
					unsigned char *arg[3] = { image, m_buffer };
					ssize_t size = stride * height;

					// Normal non-keyer playout - needs byte swapping
					if ( !m_sliced_swab )
						swab2( arg[0], arg[1], size );
					else
					{
						arg[2] = (unsigned char*)size;
						mlt_slices_run_fifo( 0, swab_sliced, arg);
					}
				}
				else if ( !mlt_properties_get_int( MLT_FRAME_PROPERTIES( frame ), "test_image" ) )
				{
					// Normal keyer output
					int y = height + 1;
					uint32_t* s = (uint32_t*) image;
					uint32_t* d = (uint32_t*) m_buffer;

					// Need to relocate alpha channel RGBA => ARGB
					while ( --y )
					{
						int x = m_width + 1;
						while ( --x )
						{
							*d++ = ( *s << 8 ) | ( *s >> 24 );
							s++;
						}
					}
				}
				else
				{
					// Keying blank frames - nullify alpha
					memset( m_buffer, 0, stride * height );
				}
			}
		}
		else if ( decklinkFrame )
		{
			uint8_t* buffer = NULL;
			decklinkFrame->GetBytes( (void**) &buffer );
			if ( buffer )
				memcpy( buffer, m_buffer, stride * height );
		}
		if ( decklinkFrame )
		{
			char* vitc;

			// set timecode
			vitc = mlt_properties_get( MLT_FRAME_PROPERTIES( frame ), "meta.attr.vitc.markup" );
			if( vitc )
			{
				int h, m, s, f;
				if ( 4 == sscanf( vitc, "%d:%d:%d:%d", &h, &m, &s, &f ) )
					decklinkFrame->SetTimecodeFromComponents(bmdTimecodeVITC,
						h, m, s, f, bmdTimecodeFlagDefault);
			}

			// set userbits
			vitc = mlt_properties_get( MLT_FRAME_PROPERTIES( frame ), "meta.attr.vitc.userbits" );
			if( vitc )
				decklinkFrame->SetTimecodeUserBits(bmdTimecodeVITC,
					mlt_properties_get_int( MLT_FRAME_PROPERTIES( frame ), "meta.attr.vitc.userbits" ));

			DeckLinkVideoFrame* decklink10BitFrame;
			if (m_supports_vanc && m_decklinkVideoConversion && decklinkFrame->GetPixelFormat() != bmdFormat10BitYUV) {
				if ( S_OK == m_deckLinkOutput->CreateVideoFrame( m_width, m_height,
					((m_width + 47)/48) * 128, bmdFormat10BitYUV, bmdFrameFlagDefault, (IDeckLinkMutableVideoFrame **) &decklink10BitFrame ) && decklink10BitFrame )
				{
					int convert_result = m_decklinkVideoConversion->ConvertFrame(decklinkFrame, decklink10BitFrame);
					mlt_log_debug(getConsumer(), "%s:%d: ConvertFrame %d\n", __FUNCTION__, __LINE__, convert_result);
					if (convert_result != S_OK) {
						decklink10BitFrame = nullptr;
					} else {
						if (create_ancillary_data(decklink10BitFrame, bmdFormat10BitYUV) == S_OK) {
							construct_vanc_cc(frame, decklink10BitFrame);
							construct_scte_104(frame, decklink10BitFrame);
						}
					}
				} else {
					mlt_log_error(getConsumer(), "%s:%d: CreateVideoFrame failed\n", __FUNCTION__, __LINE__);
				}
			}

			if (decklink10BitFrame) {
				hr = m_deckLinkOutput->ScheduleVideoFrame( decklink10BitFrame, m_count * m_duration, m_duration, m_timescale );
				decklink10BitFrame->Release();
			} else {
				hr = m_deckLinkOutput->ScheduleVideoFrame( decklinkFrame, m_count * m_duration, m_duration, m_timescale );
			}
			if ( S_OK != hr ) {
				mlt_log_error( getConsumer(), "%s:%d: ScheduleVideoFrame failed, hr=%.8X \n", __FUNCTION__, __LINE__, unsigned(hr) );
			}
			else {
				mlt_deque_push_back( m_frames_interim, decklinkFrame );
				mlt_log_debug( getConsumer(), "%s: ScheduleVideoFrame SUCCESS\n", __FUNCTION__ );
			}
		}
	}

	HRESULT render( mlt_frame frame )
	{
		HRESULT result = S_OK;

		// Get the audio
		double speed = mlt_properties_get_double( MLT_FRAME_PROPERTIES(frame), "_speed" );

		if ( m_isAudio && speed == 1.0 )
			renderAudio( frame );

		// Get the video
		renderVideo( frame );
		++m_count;

		return result;
	}

	void reprio( int target )
	{
		int r;
		pthread_t thread;
		pthread_attr_t tattr;
		struct sched_param param;
		mlt_properties properties;

		if( m_reprio & target )
			return;

		m_reprio |= target;

		properties = MLT_CONSUMER_PROPERTIES( getConsumer() );

		if ( !mlt_properties_get( properties, "priority" ) )
			return;

		pthread_attr_init(&tattr);
		pthread_attr_setschedpolicy(&tattr, SCHED_FIFO);

		if ( !strcmp( "max", mlt_properties_get( properties, "priority" ) ) )
			param.sched_priority = sched_get_priority_max(SCHED_FIFO) - 1;
		else if ( !strcmp( "min", mlt_properties_get( properties, "priority" ) ) )
			param.sched_priority = sched_get_priority_min(SCHED_FIFO) + 1;
		else
			param.sched_priority = mlt_properties_get_int( properties, "priority" );

		pthread_attr_setschedparam(&tattr, &param);

		thread = pthread_self();

		r = pthread_setschedparam(thread, SCHED_FIFO, &param);
		if( r )
			mlt_log_error( getConsumer(),
				"%s: [%d] pthread_setschedparam returned %d\n", __FUNCTION__, target, r);
		else
			mlt_log_verbose( getConsumer(),
				"%s: [%d] param.sched_priority=%d\n", __FUNCTION__, target, param.sched_priority);
	}

	// *** DeckLink API implementation of IDeckLinkVideoOutputCallback IDeckLinkAudioOutputCallback *** //

	// IUnknown needs only a dummy implementation
	virtual HRESULT STDMETHODCALLTYPE QueryInterface( REFIID iid, LPVOID *ppv )
		{ return E_NOINTERFACE; }
	virtual ULONG STDMETHODCALLTYPE AddRef()
		{ return 1; }
	virtual ULONG STDMETHODCALLTYPE Release()
		{ return 1; }

	/************************* DeckLink API Delegate Methods *****************************/

#ifdef _WIN32
	virtual HRESULT STDMETHODCALLTYPE RenderAudioSamples ( BOOL preroll )
#else
	virtual HRESULT STDMETHODCALLTYPE RenderAudioSamples ( bool preroll )
#endif
	{
		pthread_mutex_lock( &m_aqueue_lock );
		mlt_log_debug( getConsumer(), "%s: ENTERING preroll=%d, len=%d\n", __FUNCTION__, (int)preroll, mlt_deque_count( m_aqueue ));
		mlt_frame frame = (mlt_frame) mlt_deque_pop_front( m_aqueue );
		pthread_mutex_unlock( &m_aqueue_lock );

		reprio( 2 );

		if ( frame )
		{
			mlt_properties properties = MLT_FRAME_PROPERTIES( frame );
			uint64_t m_count = mlt_properties_get_int64( properties, "m_count" );
			mlt_audio_format format = mlt_audio_s16;
			int frequency = bmdAudioSampleRate48kHz;
			int samples = mlt_audio_calculate_frame_samples( m_fps, frequency, m_count );
			int16_t *pcm = 0;

			if ( !mlt_frame_get_audio( frame, (void**) &pcm, &format, &frequency, &m_inChannels, &samples ) )
			{
				HRESULT hr;
				int16_t* outBuff = NULL;
				mlt_log_debug( getConsumer(), "%s:%d, samples=%d, channels=%d, freq=%d\n",
					__FUNCTION__, __LINE__, samples, m_inChannels, frequency );

				if( m_inChannels != m_outChannels )
				{
					int s = 0;
					int c = 0;
					int size = mlt_audio_format_size( format, samples, m_outChannels );
					int16_t* src = pcm;
					int16_t* dst = (int16_t*)mlt_pool_alloc( size );
					outBuff = dst;
					for( s = 0; s < samples; s++ )
					{
						for( c = 0; c < m_outChannels; c++ )
						{
							if( c < m_inChannels )
							{
								*dst = *src;
								src++;
							}
							else
							{
								// Fill silence if there are more out channels than in channels.
								*dst = 0;
							}
						}
						for( c = 0; c < m_inChannels - m_outChannels; c++ )
						{
							// Drop samples if there are more in channels than out channels.
							src++;
						}
					}
					pcm = outBuff;
				}

#ifdef _WIN32
#define DECKLINK_UNSIGNED_FORMAT "%lu"
				unsigned long written = 0;
#else
#define DECKLINK_UNSIGNED_FORMAT "%u"
				uint32_t written = 0;
#endif
				BMDTimeValue streamTime = m_count * frequency * m_duration / m_timescale;
#ifdef _WIN32
				hr = m_deckLinkOutput->ScheduleAudioSamples( pcm, samples, streamTime, frequency, (unsigned long*) &written );
#else
				hr = m_deckLinkOutput->ScheduleAudioSamples( pcm, samples, streamTime, frequency, &written );
#endif
				if ( S_OK != hr )
					mlt_log_error( getConsumer(), "%s:%d ScheduleAudioSamples failed, hr=%.8X \n", __FUNCTION__, __LINE__, unsigned(hr) );
				else
					mlt_log_debug( getConsumer(), "%s:%d ScheduleAudioSamples success " DECKLINK_UNSIGNED_FORMAT " samples\n", __FUNCTION__, __LINE__, written );
				if ( written != (uint32_t) samples )
					mlt_log_verbose( getConsumer(), "renderAudio: samples=%d, written=" DECKLINK_UNSIGNED_FORMAT "\n", samples, written );

				mlt_pool_release( outBuff );
			}
			else
				mlt_log_error( getConsumer(), "%s:%d mlt_frame_get_audio failed\n", __FUNCTION__, __LINE__);

			mlt_frame_close( frame );

			if ( !preroll )
				RenderAudioSamples ( preroll );
		}

		if ( preroll )
			m_deckLinkOutput->StartScheduledPlayback( 0, m_timescale, 1.0 );

		return S_OK;
	}

	virtual HRESULT STDMETHODCALLTYPE ScheduledFrameCompleted( IDeckLinkVideoFrame* completedFrame, BMDOutputFrameCompletionResult completed )
	{
		mlt_log_debug( getConsumer(), "%s: ENTERING\n", __FUNCTION__ );

		// mlt_deque_push_back(m_frames, completedFrame);
		if (void *frame = mlt_deque_pop_front(m_frames_interim))
			mlt_deque_push_back(m_frames, frame);

		//  change priority of video callback thread
		reprio( 1 );

		// When a video frame has been released by the API, schedule another video frame to be output

		// ignore handler if frame was flushed
		if ( bmdOutputFrameFlushed == completed )
			return S_OK;

		// schedule next frame
		ScheduleNextFrame( false );

		// step forward frames counter if underrun
		if ( bmdOutputFrameDisplayedLate == completed )
		{
			mlt_log_verbose( getConsumer(), "ScheduledFrameCompleted: bmdOutputFrameDisplayedLate == completed\n" );
		}
		if ( bmdOutputFrameDropped == completed )
		{
			mlt_log_verbose( getConsumer(), "ScheduledFrameCompleted: bmdOutputFrameDropped == completed\n" );
			m_count++;
			ScheduleNextFrame( false );
		}

		return S_OK;
	}

	virtual HRESULT STDMETHODCALLTYPE ScheduledPlaybackHasStopped()
	{
		return mlt_consumer_is_stopped( getConsumer() ) ? S_FALSE : S_OK;
	}

	void ScheduleNextFrame( bool preroll )
	{
		// get the consumer
		mlt_consumer consumer = getConsumer();

		// Get the properties
		mlt_properties properties = MLT_CONSUMER_PROPERTIES( consumer );

		// Frame and size
		mlt_frame frame = NULL;

		mlt_log_debug( getConsumer(), "%s:%d: preroll=%d\n", __FUNCTION__, __LINE__, preroll);

		while ( !frame && (mlt_properties_get_int( properties, "running" ) || preroll ) )
		{
			mlt_log_timings_begin();
			frame = mlt_consumer_rt_frame( consumer );
			mlt_log_timings_end( NULL, "mlt_consumer_rt_frame" );
			if ( frame )
			{
				mlt_log_timings_begin();
				render( frame );
				mlt_log_timings_end( NULL, "render" );

				mlt_events_fire( properties, "consumer-frame-show", frame, NULL );

				// terminate on pause
				if ( m_terminate_on_pause &&
					mlt_properties_get_double( MLT_FRAME_PROPERTIES( frame ), "_speed" ) == 0.0 )
					stop();

				mlt_frame_close( frame );
			}
			else
				mlt_log_warning( getConsumer(), "%s: mlt_consumer_rt_frame return NULL\n", __FUNCTION__ );
		}
	}

};

/** Start the consumer.
 */

static int start( mlt_consumer consumer )
{
	// Get the properties
	mlt_properties properties = MLT_CONSUMER_PROPERTIES( consumer );
	DeckLinkConsumer* decklink = (DeckLinkConsumer*) consumer->child;
	return decklink->op( OP_START, mlt_properties_get_int( properties, "preroll" ) ) ? 0 : 1;
}

/** Stop the consumer.
 */

static int stop( mlt_consumer consumer )
{
	int r;

	mlt_log_debug( MLT_CONSUMER_SERVICE(consumer), "%s: entering\n", __FUNCTION__ );

	// Get the properties
	DeckLinkConsumer* decklink = (DeckLinkConsumer*) consumer->child;
	r = decklink->op(OP_STOP, 0);

	mlt_log_debug( MLT_CONSUMER_SERVICE(consumer), "%s: exiting\n", __FUNCTION__ );

	return r;
}

/** Determine if the consumer is stopped.
 */

static int is_stopped( mlt_consumer consumer )
{
	// Get the properties
	mlt_properties properties = MLT_CONSUMER_PROPERTIES( consumer );
	return !mlt_properties_get_int( properties, "running" );
}

/** Close the consumer.
 */

static void close( mlt_consumer consumer )
{
	mlt_log_debug( MLT_CONSUMER_SERVICE(consumer), "%s: entering\n", __FUNCTION__ );

	// Stop the consumer
	mlt_consumer_stop( consumer );

	// Close the parent
	consumer->close = NULL;
	mlt_consumer_close( consumer );

	// Free the memory
	delete (DeckLinkConsumer*) consumer->child;

	mlt_log_debug( MLT_CONSUMER_SERVICE(consumer), "%s: exiting\n", __FUNCTION__ );
}

extern "C" {

// Listen for the list_devices property to be set
static void on_property_changed( void*, mlt_properties properties, const char *name )
{
	IDeckLinkIterator* decklinkIterator = NULL;
	IDeckLink* decklink = NULL;
	IDeckLinkInput* decklinkOutput = NULL;
	int i = 0;

	if ( name && !strcmp( name, "list_devices" ) )
		mlt_event_block( (mlt_event) mlt_properties_get_data( properties, "list-devices-event", NULL ) );
	else
		return;

#ifdef _WIN32
	if ( FAILED( CoInitialize( NULL ) ) )
		return;
	if ( FAILED( CoCreateInstance( CLSID_CDeckLinkIterator, NULL, CLSCTX_ALL, IID_IDeckLinkIterator, (void**) &decklinkIterator ) ) )
		return;
#else
	if ( !( decklinkIterator = CreateDeckLinkIteratorInstance() ) )
		return;
#endif
	for ( ; decklinkIterator->Next( &decklink ) == S_OK; i++ )
	{
		if ( decklink->QueryInterface( IID_IDeckLinkOutput, (void**) &decklinkOutput ) == S_OK )
		{
			DLString name = NULL;
			if ( decklink->GetModelName( &name ) == S_OK )
			{
				char *name_cstr = getCString( name );
				const char *format = "device.%d";
				char *key = (char*) calloc( 1, strlen( format ) + 1 );

				sprintf( key, format, i );
				mlt_properties_set( properties, key, name_cstr );
				free( key );
				freeDLString( name );
				freeCString( name_cstr );
			}
			SAFE_RELEASE( decklinkOutput );
		}
		SAFE_RELEASE( decklink );
	}
	SAFE_RELEASE( decklinkIterator );
	mlt_properties_set_int( properties, "devices", i );
}

/** Initialise the consumer.
 */

mlt_consumer consumer_decklink_init( mlt_profile profile, mlt_service_type type, const char *id, char *arg )
{
	// Allocate the consumer
	DeckLinkConsumer* decklink = new DeckLinkConsumer();
	mlt_consumer consumer = NULL;

	// If allocated
	if ( !mlt_consumer_init( decklink->getConsumer(), decklink, profile ) )
	{
		// If initialises without error
		if ( decklink->op( OP_OPEN, arg? atoi(arg) : 0 ) )
		{
			consumer = decklink->getConsumer();
			mlt_properties properties = MLT_CONSUMER_PROPERTIES( consumer );

			// Setup callbacks
			consumer->close = close;
			consumer->start = start;
			consumer->stop = stop;
			consumer->is_stopped = is_stopped;
			mlt_properties_set( properties, "deinterlace_method", "onefield" );

			mlt_event event = mlt_events_listen( properties, properties, "property-changed", (mlt_listener) on_property_changed );
			mlt_properties_set_data( properties, "list-devices-event", event, 0, NULL, NULL );
		}
	}

	// Return consumer
	return consumer;
}

extern mlt_producer producer_decklink_init( mlt_profile profile, mlt_service_type type, const char *id, char *arg );

static mlt_properties metadata( mlt_service_type type, const char *id, void *data )
{
	char file[ PATH_MAX ];
	const char *service_type = NULL;
	switch ( type )
	{
		case consumer_type:
			service_type = "consumer";
			break;
		case producer_type:
			service_type = "producer";
			break;
		default:
			return NULL;
	}
	snprintf( file, PATH_MAX, "%s/decklink/%s_%s.yml", mlt_environment( "MLT_DATA" ), service_type, id );
	return mlt_properties_parse_yaml( file );
}

MLT_REPOSITORY
{
	MLT_REGISTER( consumer_type, "decklink", consumer_decklink_init );
	MLT_REGISTER( producer_type, "decklink", producer_decklink_init );
	MLT_REGISTER_METADATA( consumer_type, "decklink", metadata, NULL );
	MLT_REGISTER_METADATA( producer_type, "decklink", metadata, NULL );
}

} // extern C

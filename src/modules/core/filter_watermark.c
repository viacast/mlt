/*
 * filter_watermark.c -- watermark filter
 * Copyright (C) 2003-2014 Meltytech, LLC
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
 * License along with this library; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA  02110-1301  USA
 */

#include <framework/mlt_filter.h>
#include <framework/mlt_factory.h>
#include <framework/mlt_frame.h>
#include <framework/mlt_producer.h>
#include <framework/mlt_transition.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/** Do it :-).
*/

static int filter_get_image( mlt_frame frame, uint8_t **image, mlt_image_format *format, int *width, int *height, int writable )
{
	// Error we will return
	int error = 0;

	// Get the watermark filter object
	mlt_filter filter = mlt_frame_pop_service( frame );

	// Get the properties of the filter
	mlt_properties properties = MLT_FILTER_PROPERTIES( filter );

	mlt_service_lock( MLT_FILTER_SERVICE( filter ) );

	// Get the producer from the filter
	mlt_producer producer = mlt_properties_get_data( properties, "producer", NULL );

	// Get the composite from the filter
	mlt_transition composite = mlt_properties_get_data( properties, "composite", NULL );

	// Get the resource to use
	char *resource = mlt_properties_get( properties, "resource" );

	// Get the old resource
	char *old_resource = mlt_properties_get( properties, "_old_resource" );

	// Create a composite if we don't have one
	if ( composite == NULL )
	{
		// Create composite via the factory
		mlt_profile profile = mlt_service_profile( MLT_FILTER_SERVICE( filter ) );
		composite = mlt_factory_transition( profile, "composite", NULL );

		// Register the composite for reuse/destruction
		if ( composite != NULL )
			mlt_properties_set_data( properties, "composite", composite, 0, ( mlt_destructor )mlt_transition_close, NULL );
	}

	// If we have one
	if ( composite != NULL )
	{
		// Get the properties
		mlt_properties composite_properties = MLT_TRANSITION_PROPERTIES( composite );

		// Pass all the composite. properties on the filter down
		mlt_properties_pass( composite_properties, properties, "composite." );

		if ( mlt_properties_get( properties, "composite.out" ) == NULL )
			mlt_properties_set_int( composite_properties, "out", mlt_properties_get_int( properties, "_out" ) );

		// Force a refresh
		mlt_properties_set_int( composite_properties, "refresh", 1 );
	}

	// Create a producer if don't have one
	if ( producer == NULL || ( old_resource != NULL && strcmp( resource, old_resource ) ) )
	{
		// Get the factory producer service
		char *factory = mlt_properties_get( properties, "factory" );

		// Create the producer
		mlt_profile profile = mlt_service_profile( MLT_FILTER_SERVICE( filter ) );
		producer = mlt_factory_producer( profile, factory, resource );

		// If we have one
		if ( producer != NULL )
		{
			// Register the producer for reuse/destruction
			mlt_properties_set_data( properties, "producer", producer, 0, ( mlt_destructor )mlt_producer_close, NULL );

			// Ensure that we loop
			mlt_properties_set( MLT_PRODUCER_PROPERTIES( producer ), "eof", "loop" );

			// Set the old resource
			mlt_properties_set( properties, "_old_resource", resource );
		}
	}

	if ( producer != NULL )
	{
		// Get the producer properties
		mlt_properties producer_properties = MLT_PRODUCER_PROPERTIES( producer );

		// Now pass all producer. properties on the filter down
		mlt_properties_pass( producer_properties, properties, "producer." );
	}

	mlt_service_unlock( MLT_FILTER_SERVICE( filter ) );

	// Process all remaining filters first
	*format = mlt_image_yuv422;
	error = mlt_frame_get_image( frame, image, format, width, height, 0 );

	// Only continue if we have both producer and composite
	if ( !error && composite != NULL && producer != NULL )
	{
		// Get the service of the producer
		mlt_service service = MLT_PRODUCER_SERVICE( producer );

		// Create a temporary frame so the original stays in tact.
		mlt_frame a_frame = mlt_frame_clone( frame, 0 );

		// We will get the 'b frame' from the producer
		mlt_frame b_frame = NULL;

		// Get the original producer position
		mlt_position position = mlt_filter_get_position( filter, frame );

		// Make sure the producer is in the correct position
		mlt_producer_seek( producer, position );

		// Resetting position to appease the composite transition
		mlt_frame_set_position( a_frame, position );

		mlt_properties composite_properties = MLT_TRANSITION_PROPERTIES( composite );

		int loop = mlt_properties_get_int(properties, "loop");
		char *target_geometry = mlt_properties_get(properties, "geometry");
		char *current_geometry = mlt_properties_get(composite_properties, "geometry");

		char *transition_in_type = mlt_properties_get( properties, "transition-in.type" );
		int transition_in_length = MAX(1, mlt_properties_get_int( properties, "transition-in.length" ));

		char *transition_out_type = mlt_properties_get( properties, "transition-out.type" );
		int transition_out_length = MAX(1, mlt_properties_get_int( properties, "transition-out.length" ));
		
		int transition_in_point = -1, transition_out_point = -1;

		char *wm_in = mlt_properties_get( properties, "transition-in.point" );
		if (wm_in && strlen(wm_in)) {
			transition_in_point = mlt_properties_get_int( properties, "transition-in.point" );
		}

		char *wm_out = mlt_properties_get( properties, "transition-out.point" );
		if (wm_out && strlen(wm_out)) {
			transition_out_point = mlt_properties_get_int( properties, "transition-out.point" );
		}

		int producer_position = mlt_properties_get_int(MLT_FRAME_PROPERTIES(a_frame), "_position");
		int producer_in = mlt_properties_get_int(MLT_FRAME_PROPERTIES(a_frame), "in");
		int producer_out = mlt_properties_get_int(MLT_FRAME_PROPERTIES(a_frame), "out");

		transition_in_point = transition_in_point == -1 ? producer_in : MAX(transition_in_point, producer_in);
		transition_out_point = transition_out_point == -1 ? producer_out : MIN(transition_out_point, producer_out);

		int transitioning_in = 0;
		int transitioning_out = 0;

		while (1) {
			if (transition_in_point >= producer_out || producer_position < transition_in_point) {
				mlt_properties_set(composite_properties, "geometry", "");
				mlt_properties_set_double(properties, "wm-alpha", 0);
				goto skip;
			}
			if (producer_position > transition_out_point) {
				if (loop) {
					transition_in_point += loop;
					transition_out_point += loop;
					continue;
				}
				mlt_properties_set(composite_properties, "geometry", "");
				mlt_properties_set_double(properties, "wm-alpha", 0);
				goto skip;
			}
			break;
		}

		transitioning_in = position - transition_in_point <= transition_in_length;
		transitioning_out = !transitioning_in && (transition_out_point - position <= transition_out_length);

		int post_transition_in = position >= transition_in_point + transition_in_length;
		int pre_transition_out = position <= transition_out_point - transition_out_length;

		if (post_transition_in && pre_transition_out) {
			mlt_properties_set(composite_properties, "geometry", target_geometry);
			goto skip_transition;
		}

		int current_x = 0, current_y = 0;
		int target_x = 0, target_y = 0;
		
		if (target_geometry && strlen(target_geometry)) {
			sscanf(target_geometry, "%d/%d", &target_x, &target_y);
		}

		if (current_geometry && strlen(current_geometry)) {
			sscanf(current_geometry, "%d/%d", &current_x, &current_y);
		}

		if (!transitioning_in && !transitioning_out) {
			goto skip_transition;
		}

		if (transitioning_in && !transition_in_type) {
			goto skip_transition;
		}

		if (transitioning_out && !transition_out_type) {
			goto skip_transition;
		}

		int original_x = 0, original_y = 0;
		int fx = 0, fy = 0;
		int dx = 0, dy = 0;
		int transition_length;
		int valid_type = 0;

		if (transitioning_in) {
			transition_length = transition_in_length;
			if (!strcmp(transition_in_type, "up")) {
				valid_type = 1;
				original_x = target_x;
				original_y = target_y + (*height);
				if (!current_geometry || !strlen(current_geometry)) {
					current_x = original_x;
					current_y = original_y;
				}
				fx = 0;
				fy = current_y <= target_y ? 0 : -1;
				current_y = MAX(target_y, current_y);
			}
			if (!strcmp(transition_in_type, "down")) {
				valid_type = 1;
				original_x = target_x;
				original_y = target_y - (*height);
				if (!current_geometry || !strlen(current_geometry)) {
					current_x = original_x;
					current_y = original_y;
				}
				fx = 0;
				fy = current_y >= target_y ? 0 : 1;
				current_y = MIN(target_y, current_y);
			}
			if (!strcmp(transition_in_type, "left")) {
				valid_type = 1;
				original_x = target_x + (*width);
				original_y = target_y;
				if (!current_geometry || !strlen(current_geometry)) {
					current_x = original_x;
					current_y = original_y;
				}
				fx = current_x <= target_x ? 0 : -1;
				fy = 0;
				current_x = MAX(target_x, current_x);
			}
			if (!strcmp(transition_in_type, "right")) {
				valid_type = 1;
				original_x = target_x - (*width);
				original_y = target_y;
				if (!current_geometry || !strlen(current_geometry)) {
					current_x = original_x;
					current_y = original_y;
				}
				fx = current_x >= target_x ? 0 : 1;
				fy = 0;
				current_x = MIN(target_x, current_x);
			}
		}

		if (transitioning_out) {
			transition_length = transition_out_length;
			if (!strcmp(transition_out_type, "up")) {
				valid_type = 1;
				original_x = target_x;
				original_y = target_y;
				target_y -= *height;
				fx = 0;
				fy = current_y <= target_y ? 0 : -1;
				current_y = MAX(target_y, current_y);
			}
			if (!strcmp(transition_out_type, "down")) {
				valid_type = 1;
				original_x = target_x;
				original_y = target_y;
				target_y += *height;
				fx = 0;
				fy = current_y >= target_y ? 0 : 1;
				current_y = MIN(target_y, current_y);
			}
			if (!strcmp(transition_out_type, "left")) {
				valid_type = 1;
				original_x = target_x;
				original_y = target_y;
				target_x -= *width;
				fx = current_x <= target_x ? 0 : -1;
				fy = 0;
				current_x = MAX(target_x, current_x);
			}
			if (!strcmp(transition_out_type, "right")) {
				valid_type = 1;
				original_x = target_x;
				original_y = target_y;
				target_x += (*width);
				fx = current_x >= target_x ? 0 : 1;
				fy = 0;
				current_x = MIN(target_x, current_x);
			}
		}

		if (!valid_type || !transition_length) {
			mlt_properties_set(composite_properties, "geometry", target_geometry);
			goto skip_transition;
		}

		if (fx) {
			dx = fx * abs(original_x - target_x) / transition_length;
			if (!dx) {
				dx = fx;
			}
		}

		if (fy) {
			dy = fy * abs(original_y - target_y) / transition_length;
			if (!dy) {
				dy = fy;
			}
		}

		char new_geometry[32];
		snprintf(new_geometry, 31, "%d/%d:%s", current_x + dx, current_y + dy, "100%x100%");

		mlt_properties_set(composite_properties, "geometry", new_geometry);

skip_transition:

		// Get the b frame and process with composite if successful
		if ( mlt_service_get_frame( service, &b_frame, 0 ) == 0 )
		{
			// Get the a and b frame properties
			mlt_properties a_props = MLT_FRAME_PROPERTIES( a_frame );
			mlt_properties b_props = MLT_FRAME_PROPERTIES( b_frame );
			mlt_profile profile = mlt_service_profile( service );

			// Set the b frame to be in the same position and have same consumer requirements
			mlt_frame_set_position( b_frame, position );
			mlt_properties_set_int( b_props, "consumer_deinterlace", mlt_properties_get_int( a_props, "consumer_deinterlace" ) || mlt_properties_get_int( properties, "deinterlace" ) );

			int fade = 0;
			double dalpha = 0;

			if (transitioning_in && transition_in_type && !strcmp(transition_in_type, "fade") && transition_in_length) {
				fade = 1;
				dalpha = 1.0/(double)transition_in_length;
			}

			if (transitioning_out && transition_out_type && !strcmp(transition_out_type, "fade") && transition_out_length) {
				double current_alpha = mlt_properties_get_double(properties, "wm-alpha");
				if (!current_alpha) {
					mlt_properties_set_double(properties, "wm-alpha", 1.0);
				}
				fade = 1;
				dalpha = -1.0/(double)transition_out_length;
			}

			if (fade) {
				double current_alpha = mlt_properties_get_double(properties, "wm-alpha");
				double new_alpha = MAX(0, MIN(1, current_alpha + dalpha));
				mlt_properties_set_double(properties, "wm-alpha", new_alpha);
				mlt_filter brightness = mlt_factory_filter( NULL, "brightness", NULL );
				if (brightness) {
					mlt_properties_set_double(mlt_filter_properties(brightness), "alpha", new_alpha);
					brightness->process(brightness, b_frame);
				}
			}

			// Check for the special case - no aspect ratio means no problem :-)
			if ( mlt_frame_get_aspect_ratio( b_frame ) == 0 )
				mlt_frame_set_aspect_ratio( b_frame, mlt_profile_sar( profile ) );
			if ( mlt_frame_get_aspect_ratio( a_frame ) == 0 )
				mlt_frame_set_aspect_ratio( a_frame, mlt_profile_sar( profile ) );

			if ( mlt_properties_get_int( properties, "distort" ) )
			{
				mlt_properties_set_int( MLT_TRANSITION_PROPERTIES( composite ), "distort", 1 );
				mlt_properties_set_int( a_props, "distort", 1 );
				mlt_properties_set_int( b_props, "distort", 1 );
			}

			*format = mlt_image_yuv422;
			if ( mlt_properties_get_int( properties, "reverse" ) == 0 )
			{
				// Apply all filters that are attached to this filter to the b frame
				mlt_service_apply_filters( MLT_FILTER_SERVICE( filter ), b_frame, 0 );

				// Process the frame
				mlt_transition_process( composite, a_frame, b_frame );

				// Get the image
				error = mlt_frame_get_image( a_frame, image, format, width, height, 1 );
			}
			else
			{
				char temp[ 132 ];
				int count = 0;
				uint8_t *alpha = NULL;
				const char *rescale = mlt_properties_get( a_props, "rescale.interp" );
				if ( rescale == NULL || !strcmp( rescale, "none" ) )
					rescale = "hyper";
				mlt_transition_process( composite, b_frame, a_frame );
				mlt_properties_set_int( a_props, "consumer_deinterlace", 1 );
				mlt_properties_set_int( b_props, "consumer_deinterlace", 1 );
				mlt_properties_set( a_props, "rescale.interp", rescale );
				mlt_properties_set( b_props, "rescale.interp", rescale );
				mlt_service_apply_filters( MLT_FILTER_SERVICE( filter ), b_frame, 0 );
				error = mlt_frame_get_image( b_frame, image, format, width, height, 1 );
				alpha = mlt_frame_get_alpha_mask( b_frame );
				mlt_frame_set_image( frame, *image, *width * *height * 2, NULL );
				mlt_frame_set_alpha( frame, alpha, *width * *height, NULL );
				mlt_properties_set_int( a_props, "width", *width );
				mlt_properties_set_int( a_props, "height", *height );
				mlt_properties_set_int( a_props, "progressive", 1 );
				mlt_properties_inc_ref( b_props );
				strcpy( temp, "_b_frame" );
				while( mlt_properties_get_data( a_props, temp, NULL ) != NULL )
					sprintf( temp, "_b_frame%d", count ++ );
				mlt_properties_set_data( a_props, temp, b_frame, 0, ( mlt_destructor )mlt_frame_close, NULL );
			}
		}

skip:
		// Close the temporary frames
		mlt_frame_close( a_frame );
		mlt_frame_close( b_frame );
	}


	return error;
}

/** Filter processing.
*/

static mlt_frame filter_process( mlt_filter filter, mlt_frame frame )
{
	// Get the properties of the frame
	mlt_properties properties = MLT_FRAME_PROPERTIES( frame );

	// Assign the frame out point to the filter (just in case we need it later)
	mlt_properties_set_int( MLT_FILTER_PROPERTIES( filter ), "_out", mlt_properties_get_int( properties, "out" ) );

	// Push the filter on to the stack
	mlt_frame_push_service( frame, filter );

	// Push the get_image on to the stack
	mlt_frame_push_get_image( frame, filter_get_image );

	return frame;
}

/** Constructor for the filter.
*/

mlt_filter filter_watermark_init( mlt_profile profile, mlt_service_type type, const char *id, char *arg )
{
	mlt_filter filter = mlt_filter_new( );
	if ( filter != NULL )
	{
		mlt_properties properties = MLT_FILTER_PROPERTIES( filter );
		filter->process = filter_process;
		mlt_properties_set( properties, "factory", mlt_environment( "MLT_PRODUCER" ) );
		if ( arg != NULL )
			mlt_properties_set( properties, "resource", arg );
		// Ensure that attached filters are handled privately
		mlt_properties_set_int( properties, "_filter_private", 1 );
	}
	return filter;
}


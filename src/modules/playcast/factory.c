/*
 * factory.c -- Playcast services: pcingest (live network sources through
 * playcast-live-gateway)
 */

#include <framework/mlt.h>

extern mlt_producer producer_pcingest_init( mlt_profile profile, mlt_service_type type, const char *id, char *arg );

static mlt_properties metadata( mlt_service_type type, const char *id, void *data )
{
	char file[PATH_MAX];
	snprintf( file, PATH_MAX, "%s/playcast/%s", mlt_environment( "MLT_DATA" ), (char *) data );
	return mlt_properties_parse_yaml( file );
}

MLT_REPOSITORY
{
	MLT_REGISTER( producer_type, "pcingest", producer_pcingest_init );
	MLT_REGISTER_METADATA( producer_type, "pcingest", metadata, "producer_pcingest.yml" );
}

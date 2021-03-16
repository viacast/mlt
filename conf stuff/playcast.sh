#!/bin/bash

unit_number=$1

/viacast/playcast/sbin/start-video-preview.sh melted_preview.${unit_number} 20 >/tmp/preview_${unit_number}_log 2>/tmp/preview_${unit_number}_log_err &
/viacast/playcast/sbin/start-melted-server ${unit_number} -nodetach
#/viacast/playcast/bin/playcast-middleware

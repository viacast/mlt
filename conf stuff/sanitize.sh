#!/bin/bash

date=$(date +'%Y%m%d')

base_dir=/root/melted
source_dir=$base_dir/$date
target_dir=$base_dir/melted-playcast-$date

rm -rf $target_dir
mkdir $target_dir

cd $target_dir
mkdir -p bin etc lib sbin share

cp $source_dir/bin/melt $target_dir/bin/
cp $source_dir/bin/melted $target_dir/bin/
cp $source_dir/bin/preview-feed $target_dir/bin/

cp $source_dir/etc/*melted.conf* $target_dir/etc/

cp -RP $source_dir/lib/frei0r-1/ $target_dir/lib/
cp -RP $source_dir/lib/mlt/ $target_dir/lib/
cp -RP $source_dir/lib/pkgconfig/ $target_dir/lib/

cp -P $source_dir/lib/*.so* $target_dir/lib/

cp $source_dir/source-me $target_dir/sbin/
cp $source_dir/start-melted-server $target_dir/sbin/
cp $source_dir/stop-melted-server $target_dir/sbin/
cp $source_dir/start-video-preview.sh $target_dir/sbin/

mkdir $target_dir/share/ffmpeg
cp $source_dir/share/ffmpeg/* $target_dir/share/ffmpeg/
cp -R $source_dir/share/mlt $target_dir/share/

strip -s $target_dir/lib/*.so*
strip -s $target_dir/lib/**/*.so*

cp $source_dir/playcast.sh $target_dir/sbin/
mkdir $target_dir/backup
# tar -czvf "./melted-playcast-${date}.tar.xz" $target_dir

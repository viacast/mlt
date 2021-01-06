#!/bin/bash

date=$(date +'%Y%m%d')
source_dir=/root/melted/$date
target_dir=/root/melted/melted-playcast-$date

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

tar -czvf $target_dir.tar.xz $target_dir/

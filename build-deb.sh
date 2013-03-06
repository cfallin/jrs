#!/bin/bash

vers=2.0
wd=`pwd`
dir=`mktemp -d`

cd $dir
cp -R $wd $dir/jrs-$vers
tar jcvf jrs_$vers.orig.tar.bz2 --exclude ".*" jrs-$vers/
cd jrs-$vers
debuild
cd ..
mkdir $wd/deb/
cp jrs* $wd/deb/
cd $wd
rm -rf $dir

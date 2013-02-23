#!/bin/bash

vers=1.0
wd=`pwd`
dir=`mktemp -d`

cd $dir
cp -R $wd $dir/jrs-$vers
tar jcvf jrs_$vers.orig.tar.bz2 --exclude ".*" jrs-$vers/
cd jrs-$vers
debuild
cd ..
cp *.deb $wd/
cd $wd
rm -rf $dir

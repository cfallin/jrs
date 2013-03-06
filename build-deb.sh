#!/bin/bash

vers=2.0
wd=`pwd`
dir=`mktemp -d`

cd $dir
cp -R $wd $dir/jrs-$vers
pushd $dir/jrs-$vers
rm -rf .git* deb/
popd
tar jcvf jrs_$vers.orig.tar.bz2 jrs-$vers/
cd jrs-$vers
debuild
cd ..
mkdir -p $wd/deb/
cp jrs* $wd/deb/
cd $wd
rm -rf $dir

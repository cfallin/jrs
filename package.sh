#!/bin/bash
vers=`cat VERSION`
mkdir jrs-$vers
cd jrs-$vers
(cd ..; tar cvf - --exclude 't' --exclude '.git' .) | tar xf -
scons -c
cd ..
tar jcvf jrs-$vers.tar.bz2 jrs-$vers/
rm -rf jrs-$vers/

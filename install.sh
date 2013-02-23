#!/bin/bash

scons

installpath=/usr/local/jrs

if [ $# -lt 0 ]; then
    installpath=$1
fi

mkdir -p $installpath
cp scripts/* $installpath/
cp jrs $installpath/
chown nobody $installpath/jrs
chmod 6711 $installpath/jrs

ln -s $installpath/jrs-submit /usr/local/bin

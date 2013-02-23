#!/bin/bash

scons

installpath=/usr/local/jrs

if [ $# -lt 0 ]; then
    installpath=$1
fi

mkdir -p $installpath
cp scripts/* $installpath/
cp jrs $installpath/
chown -R nobody $installpath/
chmod 6755 $installpath/jrs

ln -s $installpath/jrs-submit /usr/local/bin

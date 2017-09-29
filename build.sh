#!/bin/bash
export CI_ROOT=$(pwd)
rm -rf musl && mkdir musl
rm -rf libevent && mkdir libevent
wget https://artifactory.twitter.biz/libs-releases-local/musl-1.1.16.tar.gz
packer fetch --use-tfe --cluster=smf1 cache-user libevent latest
tar -xzf musl-1.1.16.tar.gz -C musl --strip-components=1
cd musl
./configure --prefix=$CI_ROOT/musl/tmp --syslibdir=$CI_ROOT/musl/tmp/lib
make
make install
export PATH=$PATH:$CI_ROOT/musl/tmp/bin
cd $CI_ROOT
tar -xzf libevent-2.0.22-stable.tar.gz -C libevent --strip-components=1
cd libevent
mkdir tmp
./configure --prefix=$CI_ROOT/libevent/tmp CC=musl-gcc --enable-static --disable-shared
make
make install
cd $CI_ROOT
autoreconf -fvi
CFLAGS="-ggdb3 -O2" ./configure --enable-debug=log --with-libevent=$CI_ROOT/libevent/tmp CC=musl-gcc --enable-static
make -j

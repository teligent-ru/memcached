#!/bin/sh

[ ! -f ./configure ] && ./bootstrap.sh

[ ! -f Makefile ] && ./configure

[ ! -f memcached.spec ] && echo "memcached.spec not found." && exit 1

specfile=zmemcached.spec

version=`sed -n 's/^Version:[ ]*//p' memcached.spec`
release=`sed -n 's/^Release:[ ]*//p' memcached.spec`

cat $specfile.in | sed "s/^Version:.*/Version: $version/" | sed "s/^Release:.*/Release: $release/" > $specfile

test -f $specfile || exit 1

if [ -d ep-engine ] && [ -d .git ] && \
    ( ! [ -d ep-engine/.git ] || \
     ! [ -f ep-engine/config/autorun.sh ] ); then
  echo "Fetching ep-engine ..."
  git submodule init ep-engine && git submodule update ep-engine
fi

if [ ! -f ep-engine/config/autorun.sh ];  then
   echo "git submodule init failed for ep-engine"
   exit 1
fi

topdir=`pwd`
rpmtop=$topdir/_rpmbuild
rm -rf $rpmtop 2>/dev/null
mkdir -p $rpmtop/{SRPMS,RPMS,BUILD,SOURCES,SPECS}

make install DESTDIR=$rpmtop/BUILD || exit 1

if [ ! -f $rpmtop/BUILD/usr/local/memcached ] && [ -f $rpmtop/BUILD/usr/bin/memcached ]; then
  echo "** ERROR"
  echo "** found memcached under /usr instead of /usr/local"
  echo "** Please re-run ./configure with --prefix=/usr/local"
  exit 1
fi

if [ ! -f ep-engine/configure ]; then 
  echo "Configuring ep-engine ..."
  ( cd ep-engine && config/autorun.sh )
fi

(cd ep-engine && ( [ ! -f Makefile ] && ./configure --with-memcached=$topdir || : ) && make install DESTDIR=$rpmtop/BUILD ) || exit 1

install -Dp -m0755 scripts/memcached-tool $rpmtop/BUILD/usr/local/bin/memcached-tool
install -Dp -m0755 scripts/memcached.sysv $rpmtop/BUILD/etc/rc.d/init.d/memcached

rpmbuild --define="_topdir $rpmtop" -ba $specfile && cp $rpmtop/RPMS/`uname -p`/*.rpm . && rm -rf $rpmtop 2>/dev/null



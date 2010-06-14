#!/bin/sh

VER=4.10.0
NAME=collectd
NAMEVER=${NAME}-${VER}
SPEC=${NAMEVER}.spec
TARBALL=${NAMEVER}.tar


cd ..
git archive --prefix=collectd-${VER}/ HEAD > packaging/collectd-${VER}.tar
cd packaging

cp $SPEC ~/rpmbuild/SPECS/.
mv $TARBALL ~/rpmbuild/SOURCES/.

rpmbuild -ba ~/rpmbuild/SPECS/$SPEC

mkdir -p dist
cp ~/rpmbuild/RPMS/x86_64/collectd-*-${VER}*.rpm  dist

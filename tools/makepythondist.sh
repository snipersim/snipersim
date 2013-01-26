#!/bin/bash

set -o nounset
set -o errexit

source "$(dirname "${BASH_SOURCE}")/env_setup.sh"

PWD_ORIG=$(pwd)

for ARCH in ia32 intel64
do
	if [ $ARCH == "ia32" ]
	then
		export CC="gcc -m32"
		export LDFLAGS="-m32 -L/usr/lib -Wl,-rpath,/usr/lib -Wl,--hash-style=both"
	else
		unset CC
		export LDFLAGS="-Wl,--hash-style=both"
	fi

	TMPDIR=$(mktemp -d)
	cd $TMPDIR
	curl http://www.python.org/ftp/python/2.7.2/Python-2.7.2.tar.bz2 | tar jx -C $TMPDIR
	(
		cd Python-2.7.2
		./configure --prefix=$TMPDIR/python_kit --enable-shared --without-threads --without-signal-module
		sed -i -e 's/#define HAVE_FORKPTY 1//' pyconfig.h
		sed -i -e 's/#define HAVE_OPENPTY 1//' pyconfig.h
		make install
	)

	# Delete all kinds of stuff we don't need in the distribution
	rm -rf python_kit/share python_kit/bin
	find python_kit/lib/python2.7 -name test -exec rm -r {} \; || true
	for py in $(find python_kit/lib/python2.7 -name '*.py'); do if [ -e ${py}c ]; then rm $py; fi; done # Delete all .py for which a .pyc exists
	find python_kit/lib/python2.7 -name '*.pyo' -delete || true
	(cd python_kit/lib/python2.7; rm -rf config/libpython2.7.a distutils idlelib unittest lib-tk email lib2to3)

	# Package up and upload to exascience.elis.ugent.be
	tar czvf $PWD_ORIG/sniper-python27-$ARCH.tgz python_kit
done

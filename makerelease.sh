#!/bin/sh -e

TAG=$1
PREV_TAG=$2

git checkout refs/tags/$TAG

if [ -n "$PREV_TAG" ]; then
    git log --pretty=fuller --date=short refs/tags/$PREV_TAG..HEAD > ChangeLog
fi

autoreconf -i
./configure
make dist-bzip2
make dist-gzip
make dist-xz
make distclean

rm -f checksums.txt

VERSION=`echo -n $TAG | sed -E 's|^v([0-9]+\.[0-9]+\.[0-9]+(-[^.]+(\.[0-9]+)?)?)$|\1|'`
for f in nghttp2-v2-$VERSION.tar.bz2 nghttp2-v2-$VERSION.tar.gz nghttp2-v2-$VERSION.tar.xz; do
    sha256sum $f >> checksums.txt
    echo -n "$GPG_PASSPHRASE" | gpg --batch --passphrase-fd 0 --pinentry-mode loopback --armor --detach-sign $f
done

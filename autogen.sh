#!/bin/sh
# Run this to generate all the initial makefiles, etc.

set -e

if [ ! -f configure.ac -o ! -f COPYING ]; then
	echo "Doesn't look like you're in the source directory" >&2
	exit 1
fi

autoreconf --force --install
echo "Now type './configure ...' and 'make' to compile."

#!/bin/bash

GDB=""
#GDB="gdb --args"
BASEDIR=`pwd`/..
[ ! -d $BASEDIR/alsa-lib ] && BASEDIR="$BASEDIR/.."
CMD="$1"
shift
ALSALIB_CONF=$BASEDIR/alsa-lib/src/conf/alsa.conf

case $CMD in
run)
  PROG="$1"
  shift
  ALSA_CONFIG_PATH="$ALSALIB_CONF" \
    LD_PRELOAD=$BASEDIR/alsa-lib/src/.libs/libasound.so $GDB $PROG "$@"
  ;;
*)
  echo "This is test build using alsa-lib in: $BASEDIR"
  ./gitcompile --with-alsa-inc-prefix=$BASEDIR/alsa-lib/include \
	       --with-alsa-prefix=$BASEDIR/alsa-lib/src/.libs
  ;;
esac

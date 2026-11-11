#!/bin/zsh
# Writes every engine page, insert effect and TRK FX block as JSON, read from the real classes.
# usage: dump_params.sh <out.json> [src-root]      (needs the Debug build in build/ for its flags)
set -e
R=${0:A:h:h:h}
SRC=${2:-$R/src}
T=$(mktemp -d)
sed "s#SRCROOT#$SRC#" ${0:A:h}/dump_params.cpp > $T/dump.cpp
cd $R/build
DEFS=$(grep '^CXX_DEFINES' CMakeFiles/NeptuneTests.dir/flags.make | sed 's/^CXX_DEFINES = //')
INCS=$(grep '^CXX_INCLUDES' CMakeFiles/NeptuneTests.dir/flags.make | sed 's/^CXX_INCLUDES = //')
FLAGS=$(grep '^CXX_FLAGS = ' CMakeFiles/NeptuneTests.dir/flags.make | sed 's/^CXX_FLAGS[a-z0-9]* = //')
eval "c++ $DEFS $INCS $FLAGS -w -c $T/dump.cpp -o $T/dump.o"
LINK=$(sed "s#CMakeFiles/NeptuneTests.dir/tests/main_test.cpp.o#$T/dump.o#; s#-o [^ ]*NeptuneTests#-o $T/dump#" CMakeFiles/NeptuneTests.dir/link.txt)
eval "$LINK"
$T/dump | sed -n '/^{/,$p' > $1
rm -rf $T

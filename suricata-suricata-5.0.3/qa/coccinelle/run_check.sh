#!/bin/sh

if [ $1 ]; then
	case $1 in
	*[ch])
		LIST=$@;
		PREFIX=$(git rev-parse --show-toplevel)/
		;;
        *..*) 
        	LIST=$(git diff --pretty="format:" --name-only $1 | grep -E '[ch]$')
		PREFIX=$(git rev-parse --show-toplevel)/
		;;
	*)
		LIST=$(git show --pretty="format:" --name-only $1 | grep -E '[ch]$')
		PREFIX=$(git rev-parse --show-toplevel)/
		;;
	esac
elif git rev-parse > /dev/null 2>&1; then
	LIST=$(git ls-tree -r --name-only --full-tree  HEAD src/ | grep -E '*.c$')
	PREFIX=$(git rev-parse --show-toplevel)/
elif [ "${TOP_SRCDIR}" != "" ]; then
	LIST=$(cd ${TOP_SRCDIR} && find src -name \*.[ch])
	PREFIX=${TOP_SRCDIR}/
fi

if [ "${TOP_BUILDDIR}" != "" ]; then
	BUILT_COCCI_FILES=$(ls ${TOP_BUILDDIR}/qa/coccinelle/*.cocci)
else
	BUILT_COCCI_FILES=""
fi

if [ -z "$CONCURRENCY_LEVEL" ]; then
	CONCURRENCY_LEVEL=1
	echo "No concurrency"
else
	echo "Using concurrency level $CONCURRENCY_LEVEL"
fi

for SMPL in ${PREFIX}qa/coccinelle/*.cocci ${BUILT_COCCI_FILES}; do
	echo "Testing cocci file: $SMPL"
	if command -v parallel >/dev/null; then
		echo -n $LIST | parallel -d ' ' -j $CONCURRENCY_LEVEL spatch --very-quiet -sp_file $SMPL --undefined UNITTESTS $PREFIX{} || if [ -z "$NOT_TERMINAL" ]; then exit 1; fi
	else
		for FILE in $LIST ; do
			spatch --very-quiet -sp_file $SMPL --undefined UNITTESTS  $PREFIX$FILE || if [ -z "$NOT_TERMINAL" ]; then exit 1; fi
		done
	fi
done

exit 0

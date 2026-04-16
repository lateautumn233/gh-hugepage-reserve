#!/system/bin/sh
while [ -z "$(ls -A /data/adb/modules/)" ]; do
    sleep 1
done
DIR=/data/adb/modules/gh-hugepage-reserve
if [ -f "$DIR/disable" ]; then
	exit 0
fi
if [ -f "$DIR/stamp" ]; then
	touch "$DIR/crash"
	rm -f "$DIR/stamp"
fi
if [ -f "$DIR/crash" ]; then
	touch "$DIR/disable"
	exit 0
fi
touch "$DIR/stamp"
source "$DIR/settings.prop"
insmod "$DIR/gh_hugepage_reserve.ko" pool_target="$pool_target"
exit 0

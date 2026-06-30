#!/system/bin/sh
set -x
while [ -z "$(ls -A /data/adb/modules/)" ]; do
	sleep 1
done
DIR=/data/adb/modules/gh-hugepage-reserve
rm -f "$DIR"/{load,dmesg}.log
exec 1<>"$DIR"/load.log
exec 2<>"$DIR"/load.log
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
if ! [ -f "$DIR/settings.prop" ]; then
	echo "pool_want=1024" > "$DIR/settings.prop"
fi
source "$DIR/settings.prop"
dmesg -w &> "$DIR/dmesg.log" &
# Accept legacy pool_target= settings.prop for backward compatibility.
insmod "$DIR/gh_hugepage_reserve.ko" pool_want="${pool_want:-${pool_target:-1024}}"
exit 0

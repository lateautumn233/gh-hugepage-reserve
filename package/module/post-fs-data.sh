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
KO="$DIR/gh_hugepage_reserve.ko"
SZ="${pool_want:-${pool_target:-1024}}"

# ABI preflight: compare the running kernel's real symbol signatures (from
# /sys/kernel/btf/vmlinux) to what this .ko was built to expect, and disable any
# that drifted so a mismatched symbol is left unresolved (its feature returns
# -ENOSYS) instead of kCFI-panicking on first call. Fail-open: if the helper is
# absent or cannot read BTF, trust the compile-time version gates (old behaviour).
DIS_ARG=""
CHK="$DIR/kapi_check"
if [ -x "$CHK" ]; then
	OUT="$("$CHK" /sys/kernel/btf/vmlinux 2>>"$DIR"/load.log)"
	DISABLE="${OUT#disable=}"
	[ "$DISABLE" = "$OUT" ] && DISABLE=""	# no "disable=" line -> empty
	echo "kapi_check -> disable_kapi='${DISABLE}'"
	[ -n "$DISABLE" ] && DIS_ARG="disable_kapi=$DISABLE"
fi

# Pass BOTH size params first: a lenient kernel silently ignores the one the
# module lacks, so it still gets SZ via pool_want (v7) or pool_target (v6).
# Strict kernels reject an unknown param, so fall back to each key alone. Try
# WITH the ABI guard first, then WITHOUT it so an older .ko that predates
# disable_kapi still loads, then a bare (default-size) load.
insmod "$KO" $DIS_ARG pool_want="$SZ" pool_target="$SZ" ||
	insmod "$KO" $DIS_ARG pool_want="$SZ" ||
	insmod "$KO" $DIS_ARG pool_target="$SZ" ||
	insmod "$KO" pool_want="$SZ" pool_target="$SZ" ||
	insmod "$KO" pool_want="$SZ" ||
	insmod "$KO" pool_target="$SZ" ||
	insmod "$KO"
exit 0

set -e

REL="$(getprop ro.build.version.release)"
VER="$(getprop ro.kernel.version)"

echo "Android version: $REL"
echo "Kernel version: $VER"

mkdir -pv "$TMPDIR/$MODID"
cd "$TMPDIR/$MODID"
unzip -o "$ZIPFILE"
set -x
exec 2<>/data/tmp.txt

if [ -f /data/local/tmp/gh-hugepage-reserve.prop ]; then
	source /data/local/tmp/gh-hugepage-reserve.prop
fi

if [ -z "$KMI" ]; then
	VER="$(uname -r | sed 's/^\([0-9]*\.[0-9]*\).*/\1/')"
	if uname -r | grep -Eq -- "-android[0-9]{1,2}-"; then
		REL="$(uname -r | sed 's/.*\(android[0-9]*\).*/\1/')"
		KMI="${REL}-${VER}"
	elif [ "$(ls -1 ko/gh_hugepage_reserve/android*-$VER.ko 2>/dev/null | wc -l)" == 1 ]; then
		KMI="$(basename "$(ls -1 ko/gh_hugepage_reserve/android*-$VER.ko)" .ko)"
	else
		echo "ERROR: No match kernel module found for $(uname -r)"
		echo "Please set KMI=android?-?.? in /data/local/tmp/gh-hugepage-reserve.prop"
		exit 1
	fi
fi
echo "KMI: $KMI"

DSTDIR=/data/adb/modules_update/gh-hugepage-reserve
rm -f /data/adb/modules{_update,}/gh-hugepage-reserve/{stamp,disable}
mkdir -pv "$DSTDIR"
for mod in gh_hugepage_reserve; do
	if ! [ -f "ko/$mod/$KMI.ko" ]; then
		echo "ERROR: Kernel module $mod is not supports with $KMI"
		exit 1
	fi
	cp -v "ko/$mod/$KMI.ko" "$DSTDIR/$mod.ko"
done
cp -v module.prop "$DSTDIR/"
cp -av module/* "$DSTDIR/"
chmod +x "$DSTDIR/"*.sh
cp -a "$DSTDIR" "/data/adb/modules/"
echo
echo -e "\xe2\x9c\x85 Module installed successfully. Please reboot to take effect."
echo
exit 0

set -e

REL="$(getprop ro.build.version.release)"
VER="$(getprop ro.kernel.version)"

echo "Android version: $REL"
echo "Kernel version: $VER"

if ! uname -r | grep -q -- "-android$REL-"; then
	echo "ERROR: Current kernel is not GKI"
	exit 1
fi

KMI="android$(getprop ro.build.version.release)-$(getprop ro.kernel.version)"
echo "KMI: $KMI"
mkdir -pv "$TMPDIR/$MODID"
cd "$TMPDIR/$MODID"
unzip -o "$ZIPFILE"
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
exit 0

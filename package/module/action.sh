#!/system/bin/sh
echo "Starting DroidVM..."
sleep 1
if ! am start cn.classfun.droidvm/.ui.hugepage.HugePageActivity; then
	echo "Failed to start DroidVM, please install first"
	sleep 2
	su shell -c 'am start https://github.com/Droid-VM/DroidVM'
fi

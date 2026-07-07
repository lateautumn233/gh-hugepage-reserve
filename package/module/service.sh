#!/system/bin/sh
# crosvm's 2MB guest RAM is only backed by order-9 shmem folios (which the module
# intercepts) when shmem THP is at least "advise". Set it here, at late_start, so
# it wins over any vendor init default (often "never"). sysfs writes don't persist
# across reboot, so applying it every boot is correct. The DroidVM app also sets
# it per VM launch, so this is belt-and-suspenders / makes it app-independent.
THP=/sys/kernel/mm/transparent_hugepage/shmem_enabled
[ -e "$THP" ] && echo advise > "$THP"
/data/adb/modules/gh-hugepage-reserve/clear_stamp.sh &
exit 0

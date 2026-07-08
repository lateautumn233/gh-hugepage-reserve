#!/system/bin/sh
# Load the module with the correct preflight arguments.
#
# This is the SINGLE SOURCE OF TRUTH for "how to insmod this module": the boot
# path (post-fs-data.sh) runs it, and so does the DroidVM app when the user
# re-enables the module at runtime. Keeping one script means a runtime enable
# always reproduces the boot-time configuration - most importantly the v10 CMA
# preflight values (migrate_cma_val / pageblock_order_val), without which the
# module's whole CMA side stays off for that load - and a future change to the
# preflight lands in one place instead of drifting between the two callers.
#
# Deliberately side-effect free apart from the insmod: no crash stamp, no
# disable-file check, no log redirection, no dmesg tap. Those are boot-path
# concerns and post-fs-data.sh keeps them.
#
# Prints its preflight decisions on stdout (post-fs-data.sh redirects them into
# load.log; the app captures them). Exits with the insmod's status.
DIR=/data/adb/modules/gh-hugepage-reserve
KO="$DIR/gh_hugepage_reserve.ko"

if ! [ -f "$DIR/settings.prop" ]; then
	echo "pool_want=1024" > "$DIR/settings.prop"
fi
. "$DIR/settings.prop"

# Accept legacy pool_target= settings.prop for backward compatibility.
SZ="${pool_want:-${pool_target:-1024}}"

# ABI preflight: compare the running kernel's real symbol signatures (from
# /sys/kernel/btf/vmlinux) to what this .ko was built to expect, and disable any
# that drifted so a mismatched symbol is left unresolved (its feature returns
# -ENOSYS) instead of kCFI-panicking on first call. Also reads MIGRATE_CMA's
# value for the v10 CMA reservoir (the enumerator is config/vendor-dependent, so
# the module must not trust its build headers). Parsed per line: the output is
# multi-line (migrate_cma= + disable=). Fail-open: if the helper is absent or
# cannot read BTF, trust the compile-time version gates (old behaviour) and
# leave the reservoir off (migrate_cma_val=-1).
DIS_ARG=""
MIGRATE_CMA=""
CHK="$DIR/kapi_check"
if [ -x "$CHK" ]; then
	OUT="$("$CHK" /sys/kernel/btf/vmlinux)"
	DISABLE="$(printf '%s\n' "$OUT" | sed -n 's/^disable=//p' | head -n1)"
	MIGRATE_CMA="$(printf '%s\n' "$OUT" | sed -n 's/^migrate_cma=//p' | head -n1)"
	echo "kapi_check -> disable_kapi='${DISABLE}' migrate_cma='${MIGRATE_CMA}'"
	[ -n "$DISABLE" ] && DIS_ARG="disable_kapi=$DISABLE"
fi

# v10 CMA reservoir preflight: pageblock_order is a pure macro in the kernel
# (no variable, not in kallsyms/BTF), but /proc/pagetypeinfo prints it. Missing
# or unreadable -> -1 -> the module keeps the reservoir off (v9 path).
PB_ORDER="$(sed -n 's/^Page block order:[[:space:]]*//p' /proc/pagetypeinfo 2>/dev/null | head -n1)"
WCMA="${pool_want_with_cma:-0}"		# from settings.prop; 0 = no reservoir
# cma_probe_result=0 in settings.prop = the app's balloon probe found apps
# cannot consume CMA on this kernel: hand the module -1 preflight values so
# the whole CMA side stays cold (no boot verification, no seed blocks - the
# module is exactly v9). Any other value (unset/1) leaves it capability-based.
if [ "${cma_probe_result:-}" = "0" ]; then
	V10_ARGS="migrate_cma_val=-1 pageblock_order_val=-1 pool_want_with_cma=0"
else
	V10_ARGS="migrate_cma_val=${MIGRATE_CMA:--1} pageblock_order_val=${PB_ORDER:--1} pool_want_with_cma=${WCMA}"
fi
echo "v10 args: $V10_ARGS"

# Pass BOTH size params first: a lenient kernel silently ignores the one the
# module lacks, so it still gets SZ via pool_want (v7) or pool_target (v6).
# Strict kernels reject an unknown param, so fall back to each key alone. Try
# the full v10 set first, then WITHOUT the v10 params so an older .ko still
# loads, then without the ABI guard for a .ko that predates disable_kapi, then
# a bare (default-size) load.
insmod "$KO" $DIS_ARG pool_want="$SZ" pool_target="$SZ" $V10_ARGS ||
	insmod "$KO" $DIS_ARG pool_want="$SZ" $V10_ARGS ||
	insmod "$KO" $DIS_ARG pool_want="$SZ" pool_target="$SZ" ||
	insmod "$KO" $DIS_ARG pool_want="$SZ" ||
	insmod "$KO" $DIS_ARG pool_target="$SZ" ||
	insmod "$KO" pool_want="$SZ" pool_target="$SZ" ||
	insmod "$KO" pool_want="$SZ" ||
	insmod "$KO" pool_target="$SZ" ||
	insmod "$KO"
exit $?

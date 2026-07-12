#!/bin/sh

# Keep the RK3568 VPU awake at its highest devfreq before starting the demo.
set -eu

VPU_DEVICE=/sys/bus/platform/devices/fdf80200.rkvdec
VPU_DEVFREQ=/sys/class/devfreq/fdf80200.rkvdec
MARKER="vpu-demo-preflight-$(date +%s)-$$"

fail() {
    echo "VPU preflight failed: $*" >&2
    exit 1
}

[ "$(id -u)" = "0" ] || fail "run this script as root"
[ -w "$VPU_DEVICE/power/control" ] || fail "rkvdec power control is unavailable"
[ -w "$VPU_DEVFREQ/governor" ] || fail "rkvdec devfreq control is unavailable"

echo "<6>${MARKER}" > /dev/kmsg
echo on > "$VPU_DEVICE/power/control"
echo performance > "$VPU_DEVFREQ/governor"
sleep 2

[ "$(cat "$VPU_DEVICE/power/runtime_status")" = "active" ] || fail "rkvdec is not active"
[ "$(cat "$VPU_DEVFREQ/governor")" = "performance" ] || fail "rkvdec governor is not performance"
[ "$(cat "$VPU_DEVFREQ/cur_freq")" = "400000000" ] || fail "rkvdec did not reach 400 MHz"

if dmesg | sed -n "/${MARKER}/,\$p" | grep -E \
    'rk3x-i2c.*timeout|Cannot set voltage|_set_opp_voltage: failed|Failed to set regulator|Failed to change cpu frequency'; then
    fail "PMIC/I2C or OPP voltage error detected"
fi

echo "VPU preflight passed: active, performance, 400 MHz."
echo "You can start the video demo now."
echo "The VPU lock remains active until reboot."

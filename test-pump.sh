#!/bin/bash
# Check fan and pump control on the AORUS WATERFORCE X. Run with sudo.
# Stops coolercontrold during the test so it can't overwrite the values.
set -u

hw=$(grep -l '^waterforce$' /sys/class/hwmon/*/name 2>/dev/null | head -1 | xargs -r dirname)
[ -n "$hw" ] || { echo "waterforce hwmon device not found"; exit 1; }
[ "$(id -u)" = 0 ] || { echo "run with sudo"; exit 1; }

echo "== loaded module: $(cat /sys/module/gigabyte_waterforce/version 2>/dev/null)  hwmon: $hw"

cc_was_active=0
if systemctl is-active -q coolercontrold; then
	cc_was_active=1
	systemctl stop coolercontrold
	echo "== coolercontrold stopped for the test"
fi

show() {
	sleep 6
	printf '%-22s pump=%5s rpm pwm2=%3s [%s]  fan=%5s rpm pwm1=%3s [%s]\n' "$1" \
		"$(cat $hw/fan2_input)" "$(cat $hw/pwm2)" \
		"$(grep -o '\[[^]]*\]' $hw/pump_profile | tr -d '[]')" \
		"$(cat $hw/fan1_input)" "$(cat $hw/pwm1)" \
		"$(grep -o '\[[^]]*\]' $hw/fan_profile | tr -d '[]')"
}

try() {	# try <file> <value>
	if echo "$2" > "$hw/$1" 2>/tmp/wf_err; then show "$1=$2"
	else printf '%-22s write failed: %s\n' "$1=$2" "$(cat /tmp/wf_err)"; fi
}

echo "coolant: $(( $(cat $hw/temp1_input) / 1000 )) C"
show "baseline"
echo "-- pump through pwm2 (what CoolerControl uses)"
try pwm2 255
try pwm2 200
try pwm2 160
try pwm2 120
echo "-- fan through pwm1"
try pwm1 255
try pwm1 180
try pwm1 100
try pwm1 60
echo "-- exact RPM"
try fan1_target 1200
try fan2_target 1800

echo "== restoring automatic control"
echo balance > "$hw/pump_profile"
echo balance > "$hw/fan_profile"
[ $cc_was_active = 1 ] && systemctl start coolercontrold && echo "== coolercontrold started again"

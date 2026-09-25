#!/bin/bash
# Test for req_notify.awk: dbus-monitor's PropertiesChanged output for the REQ
# characteristic in, refresh flag / answer commands out. `answer` stands in for
# `python3 clawdmeter_info.py answer`.
set -u

AWK="$(dirname "$0")/../req_notify.awk"
TMP="$(mktemp -d)"
trap 'rm -rf "$TMP"' EXIT
fail=0

signal() {  # one PropertiesChanged signal carrying the given hex bytes
    cat <<SIG
signal time=1790346862.1 sender=:1.5 -> destination=(null destination) serial=99 path=/org/bluez/hci0/dev_28_84_85_4B_F4_60/service0028/char002f; interface=org.freedesktop.DBus.Properties; member=PropertiesChanged
   string "org.bluez.GattCharacteristic1"
   array [
      dict entry(
         string "Value"
         variant             array of bytes [
               $*
            ]
      )
   ]
   array [
   ]
SIG
}

{ signal 01; signal 02 ab 00; signal 03 01 c0; signal 07; signal 02 ab; } \
    | awk -v flag="$TMP/flag" -v answer="echo" -f "$AWK" > "$TMP/out"

check() {
    if [ "$1" = "$2" ]; then echo "PASS: $3"; else echo "FAIL: $3 (got '$1', want '$2')"; fail=1; fi
}
check "$([ -f "$TMP/flag" ] && echo yes)" "yes" "01 drops the refresh flag"
check "$(sed -n 1p "$TMP/out")" "00ab allow" "02 lo hi answers allow for id hi|lo"
check "$(sed -n 2p "$TMP/out")" "c001 deny" "03 lo hi answers deny"
check "$(wc -l < "$TMP/out" | tr -d ' ')" "2" "unknown and short notifies are ignored"
exit $fail

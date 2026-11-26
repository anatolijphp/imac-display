#!/usr/bin/env bash
nap(){ python3 -c "import time;time.sleep($1)"; }
cd /root/imac-display/receiver
/root/build/receiver/imacdisplayd --iface lo --port 7501 2> /root/build/rx.log &
RX=$!
nap 2
python3 test/fake_mac.py ::1 --port 7501 --seconds 12 --eject-after 7 > /root/build/mac.log 2>&1 &
nap 3
j=$(grep '^STORAGE_ATTACHED' /root/build/mac.log | grep imac-macos | head -1 | sed 's/^STORAGE_ATTACHED: //')
share=$(jq -r .share <<<"$j"); pw=$(jq -r .password <<<"$j")
echo "--- smbclient //127.0.0.1/$share  ls"
smbclient "//127.0.0.1/$share" -U "imac%$pw" -m SMB3 -c 'ls' 2>&1 | grep -v "Can't load"
echo "--- write attempt:"
smbclient "//127.0.0.1/$share" -U "imac%$pw" -m SMB3 -c 'put /etc/hostname imacdisplay-test.txt' 2>&1 | tail -1
echo "--- read speed (largest file in /Library, max 500 MB):"
f=$(find /run/imacdisplay/volumes/imac-macos/root/Library -type f -size +50M 2>/dev/null | head -1)
rel=${f#/run/imacdisplay/volumes/imac-macos/root/}
[ -n "$rel" ] && smbclient "//127.0.0.1/$share" -U "imac%$pw" -m SMB3 -c "get \"$rel\" /dev/null" 2>&1 | grep -i 'average\|getting'
nap 9
echo "--- still mounted after eject?"; grep imac-macos /proc/mounts
kill $RX; wait $RX 2>/dev/null
echo "--- after receiver exit:"; grep imac-macos /proc/mounts || echo "unmounted"; pgrep -a smbd || echo "no smbd"
echo "=== receiver"; grep -v '^input:' /root/build/rx.log
echo "=== fake mac"; sed -E 's/"password":"[^"]*"/"password":"…"/' /root/build/mac.log

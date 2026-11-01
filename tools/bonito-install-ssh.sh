#!/bin/bash
# Install SSH (dropbear) + dash + toybox + ios-flame's tools into the running iOS root over the file server,
# and a launchd job for dropbear (adapted from ios-flame tools/flame-install-ssh.sh, MIT). Reboot afterwards:
# launchd reads its job cache (/System/Library/xpc/launchd.plist; boot-arg launchd_unsecure_cache=1) at boot.
#   tools/bonito-install-ssh.sh [STAGE_DIR]      (default: the latest phone-tools CI artifact)
# Then: tools/bonito-ssh ['command']
set -euo pipefail
cd "$(dirname "$0")/.."
F="python3 tools/bonito-files.py"
W=work/ssh; mkdir -p $W
[ -f $W/id_ed25519 ] || ssh-keygen -q -t ed25519 -N '' -f $W/id_ed25519 -C bonito
S=${1:-}
if [ -z "$S" ]; then
	S=${TMPDIR:-/tmp}/bonito-tools; rm -rf "$S"
	id=$(gh run list -R watrabi/ios-sweet --workflow phone-tools.yml --status success -L 1 --json databaseId -q '.[0].databaseId')
	gh run download "$id" -R watrabi/ios-sweet -n phone-tools -D "$S"
fi
ping -c 1 -W 2 192.168.7.2 >/dev/null || { echo "phone not reachable: tools/bonito-usbnet.sh" >&2; exit 1; }
$F mkdir /var/local 777
$F mkdir /var/local/bin 755
$F mkdir /private/etc/dropbear 700
$F mkdir /var/root/.ssh 700
for b in "$S"/*; do $F put "$b" /var/local/bin/$(basename "$b") 755; done
$F rm /var/local/bin/dropbear 2>/dev/null || true
$F link /var/local/bin/dropbearmulti /var/local/bin/dropbear
$F link /var/local/bin/dropbearmulti /var/local/bin/dropbearkey || true
$F link /var/local/bin/dropbearmulti /var/local/bin/scp || true
for a in ls cat cp mv rm mkdir rmdir ln chmod chown ps kill grep sed head tail wc find xargs \
	sort uniq cut tr df du mount uname date env id whoami hexdump od dd sleep touch stat \
	which file tee printf echo true false test basename dirname readlink realpath uptime \
	free top hostname nc ping ifconfig netstat dmesg sync md5sum sha1sum sha256sum tar gzip less vi; do
	$F link /var/local/bin/toybox /var/local/bin/$a >/dev/null 2>&1 || true
done
$F put $W/id_ed25519.pub /var/root/.ssh/authorized_keys 600
printf 'export PATH=/var/local/bin:/usr/bin:/bin:/usr/sbin:/sbin\nexport TERM=xterm\n' > $W/profile
$F put $W/profile /var/root/.profile 644

# launchd job in launchd's cache (jobs must be keyed under /System/Library/LaunchDaemons/: ios-flame)
L=/System/Library/xpc/launchd.plist
$F get $L.orig $W/launchd.plist.orig >/dev/null 2>&1 || { $F get $L $W/launchd.plist.orig; $F put $W/launchd.plist.orig $L.orig 644; }
python3 - "$W/launchd.plist.orig" "$W/launchd.plist.new" <<'PY'
import plistlib, sys
p = plistlib.load(open(sys.argv[1], 'rb'))
p['LaunchDaemons']['/System/Library/LaunchDaemons/org.bonito.dropbear.plist'] = {
    'Label': 'org.bonito.dropbear',
    # -R: host keys made on first use; -E: log to stderr; -F: foreground for launchd
    'ProgramArguments': ['/var/local/bin/dropbear', '-F', '-E', '-R', '-p', '22'],
    'RunAtLoad': True, 'KeepAlive': True, 'UserName': 'root',
    'EnvironmentVariables': {'PATH': '/var/local/bin:/usr/bin:/bin:/usr/sbin:/sbin', 'HOME': '/var/root'},
    'StandardErrorPath': '/var/local/dropbear.log',
}
plistlib.dump(p, open(sys.argv[2], 'wb'), fmt=plistlib.FMT_BINARY)
print('launchd jobs:', len(p['LaunchDaemons']))
PY
$F put $W/launchd.plist.new $L 644
python3 tools/bonito-cmd.py sync >/dev/null 2>&1 || true
echo "installed; reboot (tools/bonito-cmd.py reboot), boot again, then tools/bonito-ssh"

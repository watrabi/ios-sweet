#!/bin/bash
# Rewrite the phone's launchd job cache (/System/Library/xpc/launchd.plist) from the saved original
# (launchd.plist.orig on the phone, work/ssh/launchd.plist.orig here): + dropbear, - the jobs below.
# Takes effect at the next boot. Needs the file server (USB=2 image).
#   tools/bonito-launchd.sh [LABEL...]     extra labels to leave out
# Left out by default:
#   com.apple.voicemail.vmd   visual voicemail: with no modem it loops on CoreTelephony errors (30k log lines in 10 s,
#                   most of a CPU: 384 CPU-seconds in a few minutes)
set -euo pipefail
cd "$(dirname "$0")/.."
F="python3 tools/bonito-files.py"
W=work/ssh; mkdir -p $W
L=/System/Library/xpc/launchd.plist
[ -s $W/launchd.plist.orig ] || $F get $L.orig $W/launchd.plist.orig
python3 - "$W/launchd.plist.orig" "$W/launchd.plist.new" com.apple.voicemail.vmd "$@" <<'PY'
import plistlib, sys
p = plistlib.load(open(sys.argv[1], 'rb'))
p['LaunchDaemons']['/System/Library/LaunchDaemons/org.bonito.dropbear.plist'] = {
    'Label': 'org.bonito.dropbear',
    'ProgramArguments': ['/var/local/bin/dropbear', '-F', '-R', '-p', '22'],
    'RunAtLoad': True, 'KeepAlive': True, 'UserName': 'root',
    'EnvironmentVariables': {'PATH': '/var/local/bin:/usr/bin:/bin:/usr/sbin:/sbin', 'HOME': '/var/root'},
    'StandardErrorPath': '/var/local/dropbear.log',
}
drop = set(sys.argv[3:])
for kind in ('LaunchDaemons', 'LaunchAgents'):
    for k in [k for k, v in p.get(kind, {}).items() if isinstance(v, dict) and v.get('Label') in drop]:
        print('leaving out', kind, p[kind][k]['Label'])
        del p[kind][k]
plistlib.dump(p, open(sys.argv[2], 'wb'), fmt=plistlib.FMT_BINARY)
print('launchd jobs:', len(p['LaunchDaemons']))
PY
$F put $W/launchd.plist.new $L 644
python3 tools/bonito-cmd.py sync >/dev/null 2>&1 || true
echo "written; takes effect at the next boot"

#!/usr/bin/env python3
"""bonito-profile.py PROC [SAMPLES]: sample a phone process's thread stacks with pstack (userland/tools) over SSH
while you use the phone, then print where the time goes: per thread, the leaf library/function and the
inclusive libraries. Symbols: SweetMetal2 and Turnip from their Mach-O symbol tables (bases from
/var/local/mtl2.log "images:"), Apple libraries by image from the shared cache in the root image
(mounted read-only at /mnt/iosro). Linux port of ios-flame's tools/flame-profile.py idea (no atos/ipsw here).
"""
import bisect, collections, glob, os, re, struct, subprocess, sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
CACHE = '/mnt/iosro/System/Library/Caches/com.apple.dyld/dyld_shared_cache_arm64'


def ssh(cmd):
    return subprocess.run([ROOT + '/tools/bonito-ssh', cmd], capture_output=True, text=True).stdout


def macho_syms(path, base):
    """(addr, name) of the defined section symbols of a thin arm64 Mach-O, rebased so __TEXT starts at base."""
    d = open(path, 'rb').read()
    magic, _, _, _, ncmds, _, _, _ = struct.unpack_from('<IiiIIIII', d, 0)
    assert magic == 0xfeedfacf, path
    off, text_vm, symtab = 32, None, None
    for _ in range(ncmds):
        cmd, size = struct.unpack_from('<II', d, off)
        if cmd == 0x19 and d[off + 8:off + 24].rstrip(b'\0') == b'__TEXT':
            text_vm = struct.unpack_from('<Q', d, off + 24)[0]
        elif cmd == 0x2:
            symtab = struct.unpack_from('<IIII', d, off + 8)
        off += size
    out = []
    if symtab:
        symoff, nsyms, stroff, _ = symtab
        for i in range(nsyms):
            strx, ntype, sect, _, value = struct.unpack_from('<IBBHQ', d, symoff + 16 * i)
            if ntype & 0xe0 or (ntype & 0x0e) != 0x0e or not sect:      # skip stabs; N_SECT only
                continue
            name = d[stroff + strx:d.index(b'\0', stroff + strx)].decode(errors='replace')
            out.append((value - text_vm + base, name))
    return sorted(out)


def cache_images(slide):
    """(address, path) of every shared cache image, slid."""
    d = open(CACHE, 'rb').read(0x200000)
    imgs_off, imgs_n = struct.unpack_from('<II', d, 0x1c0)
    if imgs_n == 0:
        imgs_off, imgs_n = struct.unpack_from('<II', d, 0x18)
    out = []
    for i in range(imgs_n):
        addr, _, _, pathoff, _ = struct.unpack_from('<QQQII', d, imgs_off + 32 * i)
        path = d[pathoff:d.index(b'\0', pathoff)].decode()
        out.append((addr + slide, os.path.basename(path)))
    return sorted(out)


def main():
    proc, n = sys.argv[1], int(sys.argv[2]) if len(sys.argv) > 2 else 60
    pid = ssh('psx %s' % proc).split()[0]
    imgs = [l for l in ssh('grep "images:" /var/local/mtl2.log').splitlines() if '[%s ' % pid in l]
    print('sampling %s (pid %s) %d times: use the phone now' % (proc, pid, n), flush=True)
    raw = ssh('i=0; while [ $i -lt %d ]; do pstack %s; i=$((i+1)); done' % (n, pid))
    os.makedirs(ROOT + '/work/prof', exist_ok=True)
    open(ROOT + '/work/prof/%s-raw.txt' % proc, 'w').write(raw)
    slide = int(re.search(r'slide 0x([0-9a-f]+)', raw).group(1), 16)
    syms = []
    if imgs:
        sm2, tu = (int(x, 16) for x in re.findall(r'(0x[0-9a-f]+)', imgs[-1])[:2])
        b = glob.glob(os.environ.get('TMPDIR', '/tmp') + '/bonito-metal/metal2/out/SweetMetal2.bundle')
        if b:
            syms += [(a, 'SweetMetal2:' + s) for a, s in macho_syms(b[0] + '/SweetMetal2', sm2)]
            syms += [(a, 'Turnip:' + s) for a, s in macho_syms(b[0] + '/libvulkan_freedreno.dylib', tu)]
    syms.sort()
    lib = cache_images(slide) if os.path.exists(CACHE) else []
    sa, la = [a for a, _ in syms], [a for a, _ in lib]

    def name(a):
        i = bisect.bisect_right(sa, a) - 1
        if i >= 0 and a - sa[i] < 0x40000:
            return syms[i][1]
        j = bisect.bisect_right(la, a) - 1
        if j >= 0 and lib[0][0] <= a < lib[-1][0] + 0x10000000:
            return lib[j][1]
        return 'main/other'

    threads = collections.defaultdict(list)
    for block in raw.split('shared cache slide')[1:]:
        for m in re.finditer(r'thread (\d+): pc (0x[0-9a-f]+) lr 0x[0-9a-f]+\n  bt ([0-9a-fx ]*)', block):
            threads[int(m.group(1))].append([int(m.group(2), 16)] + [int(x, 16) for x in m.group(3).split()])
    waiting = ('libsystem_kernel.dylib',)
    for t, stacks in sorted(threads.items()):
        leaves = collections.Counter(name(s[0]) for s in stacks)
        busy = sum(c for k, c in leaves.items() if k not in waiting)
        if busy < max(3, len(stacks) // 10):
            continue
        print('\n== thread %d: busy in %d of %d samples' % (t, busy, len(stacks)))
        for k, c in leaves.most_common(8):
            print('  leaf %4d  %s' % (c, k))
        incl = collections.Counter()
        for s in stacks:
            if name(s[0]) in waiting:
                continue
            for k in {name(a).split(':')[0] for a in s}:
                incl[k] += 1
        print('  inclusive (busy samples): ' + ', '.join('%s %d' % kv for kv in incl.most_common(8)))


if __name__ == '__main__':
    main()

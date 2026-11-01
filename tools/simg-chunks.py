#!/usr/bin/env python3
"""Flash a big raw image in bounded sparse pieces, in one streaming pass.

  simg-chunks.py IMAGE PARTITION [--max-raw MIB] [--tmp DIR]
      fastboot flash PARTITION piece by piece (each deleted after flashing)
  simg-chunks.py IMAGE --dry-run OUTDIR [--max-raw MIB]
      only write the pieces (piece-000.simg, ...) for checking

No full .simg copy is ever on disk: at most one piece (<= --max-raw MiB of
data, default 200, below bonito's 256 MiB max-download-size).

Every piece describes the WHOLE image (same total block count): DONT_CARE
before and after its slice; inside the slice RAW chunks for data blocks and
FILL (value 0) chunks for all-zero runs. Flashing all pieces therefore
writes every block of IMAGE exactly once (zero blocks really get zeroed, as
with img2simg) and nothing beyond it. Reads only IMAGE.
"""
import argparse, os, struct, subprocess, sys

MAGIC = 0xED26FF3A
RAW, FILL, DONT_CARE = 0xCAC1, 0xCAC2, 0xCAC3
FILE_HDR, CHUNK_HDR = 28, 12
STEP = 8 << 20
MAX_RAW_CHUNK_BLOCKS = 16384        # 64 MiB per RAW chunk


def is_zero(b):
    return b.count(0) == len(b)


def emit(path, bs, nblocks, start, end, chunks):
    """chunks: list of (type, nblocks, payload) covering [start, end)."""
    allc = ([(DONT_CARE, start, None)] if start else []) + chunks + \
           ([(DONT_CARE, nblocks - end, None)] if end < nblocks else [])
    assert sum(c[1] for c in allc) == nblocks
    with open(path + '.tmp', 'wb') as o:
        o.write(struct.pack('<IHHHHIIII', MAGIC, 1, 0, FILE_HDR, CHUNK_HDR, bs, nblocks, len(allc), 0))
        for t, n, payload in allc:
            if t == RAW:
                o.write(struct.pack('<HHII', t, 0, n, CHUNK_HDR + len(payload)))
                o.write(payload)
            elif t == FILL:
                o.write(struct.pack('<HHII', t, 0, n, CHUNK_HDR + 4) + b'\0\0\0\0')
            else:
                o.write(struct.pack('<HHII', t, 0, n, CHUNK_HDR))
    os.replace(path + '.tmp', path)


def pieces(image, bs, max_raw):
    """Yield (start, end, chunks) slices of the image, one streaming pass."""
    size = os.path.getsize(image)
    if size % bs:
        sys.exit('image size must be a multiple of the block size')
    nblocks = size // bs
    start, raw, chunks, cur = 0, 0, [], None

    def close_cur():
        nonlocal cur
        if cur:
            chunks.append((cur[0], cur[1], b''.join(cur[2]) if cur[0] == RAW else None))
            cur = None

    with open(image, 'rb') as f:
        blk = 0
        while blk < nblocks:
            buf = f.read(min(STEP, (nblocks - blk) * bs))
            for i in range(len(buf) // bs):
                data = buf[i * bs:(i + 1) * bs]
                t = FILL if is_zero(data) else RAW
                if t == RAW and raw + bs > max_raw:
                    close_cur()
                    yield start, blk, chunks
                    start, raw, chunks = blk, 0, []
                if cur and cur[0] == t and (t == FILL or cur[1] < MAX_RAW_CHUNK_BLOCKS):
                    cur[1] += 1
                    if t == RAW:
                        cur[2].append(data)
                else:
                    close_cur()
                    cur = [t, 1, [data] if t == RAW else []]
                if t == RAW:
                    raw += bs
                blk += 1
    close_cur()
    yield start, nblocks, chunks


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('image')
    ap.add_argument('partition', nargs='?')
    ap.add_argument('--dry-run', metavar='OUTDIR')
    ap.add_argument('--max-raw', type=int, default=200)
    ap.add_argument('--tmp', default='/tmp')
    a = ap.parse_args()
    if not a.partition and not a.dry_run:
        ap.error('need PARTITION or --dry-run OUTDIR')
    bs = 4096
    nblocks = os.path.getsize(a.image) // bs
    for i, (s, e, chunks) in enumerate(pieces(a.image, bs, a.max_raw << 20)):
        raw = sum(len(c[2]) for c in chunks if c[0] == RAW)
        out = os.path.join(a.dry_run or a.tmp, f'piece-{i:03d}.simg')
        emit(out, bs, nblocks, s, e, chunks)
        print(f'piece {i}: blocks {s}..{e} ({(e - s) * bs >> 20} MiB span, {raw >> 20} MiB data)', flush=True)
        if a.dry_run:
            continue
        r = subprocess.run(['fastboot', 'flash', a.partition, out])
        os.remove(out)
        if r.returncode:
            sys.exit(f'fastboot failed on piece {i} (blocks {s}..{e}); rerun is safe: '
                     'pieces are independent and idempotent')
    print('all pieces flashed' if not a.dry_run else 'dry run done')


if __name__ == '__main__':
    main()

# zip_unwrap.py list|test|extract <zip> [<out dir> <glob>...]
# Reads zip archives larger than 4 GiB that were written without Zip64, such as the
# 2008 Fairchild HDRPS archive. Their 32-bit file offsets wrap around every 4 GiB,
# so unzip reports the wrapped part as "extra bytes at beginning or within zipfile"
# and Python's zipfile shifts every entry by the same amount, which is wrong for
# all but the last 4 GiB. Here the offsets are unwrapped in central-directory
# order, and each one is confirmed by the local header signature found there.
#   list     names, sizes and true offsets
#   test     decompresses every entry and checks its CRC-32: the corruption test
#   extract  writes the entries matching any glob (fnmatch on the full name)
import sys, os, struct, zlib, fnmatch

WRAP = int(os.environ.get('ZIP_UNWRAP_MODULUS', str(1 << 32)))   # smaller only for testing


def entries(f, size):
    # the end-of-central-directory record is the last thing in the file, before a
    # comment of at most 65535 bytes
    f.seek(max(0, size - 65557))
    tail = f.read()
    e = tail.rfind(b'PK\x05\x06')
    if e < 0:
        sys.exit('no end-of-central-directory record: the file is truncated')
    eocd = size - len(tail) + e
    n, cd_size, cd_off = struct.unpack('<HII', tail[e + 10:e + 20])
    if cd_off == 0xFFFFFFFF:
        sys.exit('this is a Zip64 archive; use unzip or python3 -m zipfile')
    # the central directory ends where the end record starts; its stored offset wrapped
    cd_start = eocd - cd_size
    if cd_start % WRAP != cd_off % WRAP:
        sys.exit('central directory offset does not match modulo %d: real damage' % WRAP)
    f.seek(cd_start)
    cd = f.read(cd_size)
    out, p, prev = [], 0, 0
    for _ in range(n):
        if cd[p:p + 4] != b'PK\x01\x02':
            sys.exit('bad central directory entry at %d' % (cd_start + p))
        (flags, method, crc, csize, usize, nlen, xlen, clen,
         off) = struct.unpack('<4xHH4xIIIHHH8xI', cd[p + 4:p + 46])
        name = cd[p + 46:p + 46 + nlen].decode('utf-8' if flags & 0x800 else 'cp437')
        p += 46 + nlen + xlen + clen
        # entries are stored in file order: take the smallest unwrapped offset not
        # before the previous one, and step a further 4 GiB if no header is there
        true = off + (prev // WRAP) * WRAP
        if true < prev:
            true += WRAP
        while True:
            if true >= cd_start:
                sys.exit('no local header found for %s' % name)
            f.seek(true)
            if f.read(4) == b'PK\x03\x04':
                break
            true += WRAP
        f.seek(true + 26)
        ln, lx = struct.unpack('<HH', f.read(4))
        out.append(dict(name=name, method=method, crc=crc, csize=csize, usize=usize,
                        offset=true, data=true + 30 + ln + lx))
        prev = true
    return out


def read(f, e):
    f.seek(e['data'])
    raw = f.read(e['csize'])
    if e['method'] == 0:
        data = raw
    elif e['method'] == 8:
        data = zlib.decompress(raw, -15)
    else:
        raise ValueError('compression method %d' % e['method'])
    ok = len(data) == e['usize'] and zlib.crc32(data) == e['crc']
    return data, ok


def main():
    cmd, path = sys.argv[1], sys.argv[2]
    size = os.path.getsize(path)
    with open(path, 'rb') as f:
        es = entries(f, size)
        if cmd == 'list':
            for e in es:
                print('%12d %12d  %s' % (e['offset'], e['usize'], e['name']))
            print('%d entries, last data ends at %d of %d bytes'
                  % (len(es), max(e['data'] + e['csize'] for e in es), size))
        elif cmd == 'test':
            bad = 0
            for e in es:
                if e['name'].endswith('/'):
                    continue
                try:
                    ok = read(f, e)[1]
                except Exception as x:
                    ok = False
                    print('error', e['name'], x)
                if not ok:
                    bad += 1
                    print('BAD', e['name'])
            print('%d entries tested, %d bad' % (len(es), bad))
            sys.exit(1 if bad else 0)
        elif cmd == 'extract':
            dst, globs = sys.argv[3], sys.argv[4:]
            for e in es:
                if e['name'].endswith('/') or not any(fnmatch.fnmatch(e['name'], g) for g in globs):
                    continue
                # never write outside the target directory
                rel = os.path.normpath(e['name']).lstrip('/')
                if rel.startswith('..'):
                    print('skipped unsafe name', e['name'])
                    continue
                data, ok = read(f, e)
                if not ok:
                    print('BAD, not written:', e['name'])
                    continue
                out = os.path.join(dst, rel)
                os.makedirs(os.path.dirname(out), exist_ok=True)
                with open(out, 'wb') as o:
                    o.write(data)
                print('extracted', rel)


if __name__ == '__main__':
    main()

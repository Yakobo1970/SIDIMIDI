"""Writes small .7z archives for testing SevenZip.cpp (no 7-Zip needed).
usage: python make7z.py <out.7z> <method: lzma2|lzma> <header: plain|encoded> <dict_size> <folder-with-files> [empty]
All files below <folder> are stored in ONE solid block; with 'empty' an empty file and an empty directory are added."""
import lzma, os, struct, sys, zlib

def num(n):
    # 7z variable length number
    if n < 0x80: return bytes([n])
    for extra in range(1, 9):
        if n < (1 << (7 * (extra + 1))) or extra == 8:
            if extra == 8:
                return b'\xff' + n.to_bytes(8, 'little')
            first = (0xFF << (8 - extra)) & 0xFF
            high = n >> (8 * extra)
            return bytes([first | high]) + (n & ((1 << (8 * extra)) - 1)).to_bytes(extra, 'little')
    raise ValueError

def crc(b): return zlib.crc32(b) & 0xffffffff

def compress(data, method, dict_size):
    if method == 'lzma2':
        raw = lzma.compress(data, format=lzma.FORMAT_RAW, filters=[{"id": lzma.FILTER_LZMA2, "preset": 5, "dict_size": dict_size}])
        db = 0
        # dict size byte: dict = (2 | (b & 1)) << (b / 2 + 11)
        for b in range(41):
            if ((2 | (b & 1)) << (b // 2 + 11)) >= dict_size: db = b; break
        return raw, 0x21, bytes([db])
    raw = lzma.compress(data, format=lzma.FORMAT_RAW, filters=[{"id": lzma.FILTER_LZMA1, "preset": 5, "dict_size": dict_size, "lc": 3, "lp": 0, "pb": 2}])
    props = bytes([(2 * 5 + 0) * 9 + 3]) + struct.pack('<I', dict_size)
    return raw, 0x030101, props

def coder_bytes(method_id, props):
    idb = method_id.to_bytes(max(1, (method_id.bit_length() + 7) // 8), 'big')
    return bytes([len(idb) | 0x20]) + idb + num(len(props)) + props

def bitvec(bits):
    out = bytearray((len(bits) + 7) // 8)
    for i, b in enumerate(bits):
        if b: out[i // 8] |= 0x80 >> (i % 8)
    return bytes(out)

def main():
    out, method, header_mode, dict_size, folder = sys.argv[1], sys.argv[2], sys.argv[3], int(sys.argv[4]), sys.argv[5]
    add_empty = 'empty' in sys.argv[6:]
    files = []
    for root, dirs, names in os.walk(folder):
        for n in sorted(names):
            p = os.path.join(root, n)
            files.append((os.path.relpath(p, folder).replace(os.sep, '/'), open(p, 'rb').read()))
    files.sort()
    blob = b''.join(d for _, d in files)
    packed, mid, props = compress(blob, method, dict_size)

    # files info: streams first, then (optional) empty file and empty dir
    names = [n for n, _ in files]
    empties = []
    if add_empty: empties = [('leer.txt', True), ('leerer/ordner', False)]
    all_names = names + [n for n, _ in empties]
    emptyStream = [False] * len(names) + [True] * len(empties)
    emptyFile = [isf for _, isf in empties]

    h = bytearray([0x01, 0x04])
    # pack info
    h += bytes([0x06]) + num(0) + num(1) + bytes([0x09]) + num(len(packed)) + bytes([0x00])
    # unpack info
    h += bytes([0x07, 0x0B]) + num(1) + b'\x00' + num(1) + coder_bytes(mid, props) + bytes([0x0C]) + num(len(blob)) + bytes([0x00])
    # sub streams
    h += bytes([0x08, 0x0D]) + num(len(files))
    if len(files) > 1:
        h += bytes([0x09]) + b''.join(num(len(d)) for _, d in files[:-1])
    h += bytes([0x0A, 0x01]) + b''.join(struct.pack('<I', crc(d)) for _, d in files) + bytes([0x00])
    h += bytes([0x00])   # end streams info
    # files info
    h += bytes([0x05]) + num(len(all_names))
    if add_empty:
        ev = bitvec(emptyStream); h += bytes([0x0E]) + num(len(ev)) + ev
        fv = bitvec(emptyFile); h += bytes([0x0F]) + num(len(fv)) + fv
    nm = b''.join(n.encode('utf-16-le') + b'\x00\x00' for n in all_names)
    h += bytes([0x11]) + num(len(nm) + 1) + b'\x00' + nm
    h += bytes([0x00, 0x00])
    header = bytes(h)

    body = packed
    if header_mode == 'encoded':
        hp, hmid, hprops = compress(header, 'lzma', 1 << 16)
        eh = bytearray([0x17])
        eh += bytes([0x06]) + num(len(packed)) + num(1) + bytes([0x09]) + num(len(hp)) + bytes([0x00])
        eh += bytes([0x07, 0x0B]) + num(1) + b'\x00' + num(1) + coder_bytes(hmid, hprops) + bytes([0x0C]) + num(len(header))
        eh += bytes([0x0A, 0x01]) + struct.pack('<I', crc(header)) + bytes([0x00, 0x00])
        body = packed + hp
        header = bytes(eh)
    start = struct.pack('<QQI', len(body), len(header), crc(header))
    sig = b'7z\xbc\xaf\x27\x1c' + bytes([0, 4]) + struct.pack('<I', crc(start)) + start
    with open(out, 'wb') as f:
        f.write(sig + body + header)
    print(out, len(files), 'files', len(blob), 'bytes ->', len(body), 'packed')

main()

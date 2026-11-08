# SPDX-License-Identifier: MIT
"""Independent VFAT wire fixtures layered onto the existing sparse FAT builder.

Tree keys remain explicit short aliases; names maps their absolute paths to LFNs.
The short entries, FAT allocation and payloads come from fatgen, not the reader.
"""
import struct
import fatgen

OFFSETS = (1, 3, 5, 7, 9, 14, 16, 18, 20, 22, 24, 28, 30)

def checksum(alias):
    value = 0
    for byte in alias:
        value = (((value & 1) << 7) + (value >> 1) + byte) & 255
    return value

def records(name, alias):
    encoded = name.encode('utf-16le', errors='surrogatepass')
    units = list(struct.unpack('<' + 'H' * (len(encoded)//2), encoded))
    count = (len(units) + 12) // 13
    if len(units) % 13:
        units.append(0)
    units += [0xffff] * (count * 13 - len(units))
    result = []
    for ordinal in range(count, 0, -1):
        entry = bytearray(32)
        entry[0] = ordinal | (0x40 if ordinal == count else 0)
        entry[11] = 15
        entry[13] = checksum(alias)
        for offset, unit in zip(OFFSETS, units[(ordinal-1)*13:ordinal*13]):
            struct.pack_into('<H', entry, offset, unit)
        result.append(bytes(entry))
    return result

def build(path, tree, names, spc=1, fragment=True):
    # Reserve enough real directory slots before fatgen allocates its chains.
    def padded(items, prefix=''):
        out = {}
        for alias, item in items.items():
            full = prefix + '/' + alias
            out[alias] = padded(item, full) if isinstance(item, dict) else item
        extra = sum(len(records(names[prefix+'/'+a], fatgen.short(a)))
                    for a in items if prefix+'/'+a in names)
        out.update({f'Z{i:07}': b'' for i in range(extra)})
        return out
    volume = fatgen.build(path, padded(tree), spc=spc, fragment=fragment, extras=False)
    def rewrite(items, prefix=''):
        chain = volume.chains[prefix or '/']
        with open(path, 'rb') as f:
            raw = []
            if prefix:
                f.seek(volume.lba(chain[0])*512)
                raw = [f.read(32), f.read(32)]
            for alias, item in items.items():
                full = prefix + '/' + alias
                f.seek(volume.entry_offset(full))
                short_entry = f.read(32)
                if full in names:
                    raw.extend(records(names[full], short_entry[:11]))
                raw.append(short_entry)
                volume.entries[full] = (chain, len(raw)-1)
        volume.put(chain, b''.join(raw))
        for alias, item in items.items():
            if isinstance(item, dict): rewrite(item, prefix+'/'+alias)
    rewrite(tree)
    with open(path, 'r+b') as f:
        for lba, data in volume.sectors.items():
            f.seek(lba*512); f.write(data)
    return volume

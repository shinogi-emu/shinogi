"""Compute the font IDs fVDI's ft2 module assigns, without running the guest.

ft2_get_face_id() hashes FreeType's family_name + style_name as a wrapping
16-bit short, adds 5000, folds negatives, then increments at least once and
keeps incrementing past any ID already registered.  FreeType takes those two
names from the TrueType 'name' table, IDs 1 and 2, preferring the Windows
platform record.  So the whole thing is reproducible on the host.
"""
import os
import struct
import sys


def _decode(data, platform_id, encoding_id):
    # Windows records are UTF-16BE; Macintosh Roman is close enough to latin-1
    # for the family names we ship.
    if platform_id == 3 or (platform_id == 0):
        try:
            return data.decode("utf-16-be")
        except UnicodeDecodeError:
            return None
    try:
        return data.decode("latin-1")
    except UnicodeDecodeError:
        return None


def read_names(path):
    """Return (family, style) the way FreeType would report them."""
    with open(path, "rb") as fh:
        blob = fh.read()

    if len(blob) < 12:
        return None
    tag = blob[:4]
    if tag == b"ttcf":
        offset = struct.unpack_from(">I", blob, 12)[0]
    else:
        offset = 0

    num_tables = struct.unpack_from(">H", blob, offset + 4)[0]
    name_off = name_len = None
    for i in range(num_tables):
        rec = offset + 12 + i * 16
        if blob[rec:rec + 4] == b"name":
            name_off, name_len = struct.unpack_from(">II", blob, rec + 8)
            break
    if name_off is None:
        return None

    count, string_off = struct.unpack_from(">HH", blob, name_off + 2)
    best = {}
    for i in range(count):
        rec = name_off + 6 + i * 12
        pid, eid, lid, nid, ln, off = struct.unpack_from(">HHHHHH", blob, rec)
        if nid not in (1, 2):
            continue
        start = name_off + string_off + off
        text = _decode(blob[start:start + ln], pid, eid)
        if not text:
            continue
        # Prefer the Windows/English record, which is what FreeType picks.
        rank = 0 if (pid == 3 and lid == 0x409) else 1
        if nid not in best or rank < best[nid][0]:
            best[nid] = (rank, text)

    if 1 not in best:
        return None
    return best[1][1], best.get(2, (1, "Regular"))[1]


def hash_id(family, style):
    hc = 0
    for ch in family + style:
        hc = (hc << 5) - hc + ord(ch)
        hc = ((hc + 0x8000) & 0xFFFF) - 0x8000   # wrap as int16
    ident = hc + 5000
    ident = ((ident + 0x8000) & 0xFFFF) - 0x8000
    if ident < 0:
        ident = -ident
    if ident < 5000:
        ident += 5000
    return ident


def main(root):
    # fVDI walks with Fsfirst/Fsnext and recurses into directories, so sort to
    # approximate that order; it only matters for collision resolution.
    files = []
    for dirpath, dirnames, filenames in os.walk(root):
        dirnames.sort()
        for fn in sorted(filenames):
            if fn.lower().endswith(".ttf"):
                files.append(os.path.join(dirpath, fn))

    taken = set()
    rows = []
    for path in files:
        names = read_names(path)
        if not names:
            rows.append((None, "?unreadable", os.path.relpath(path, root)))
            continue
        family, style = names
        ident = hash_id(family, style)
        while True:                      # the do/while: always increments once
            ident += 1
            if ident not in taken:
                break
        taken.add(ident)
        rows.append((ident, "%s %s" % (family, style), os.path.relpath(path, root)))

    for ident, label, rel in sorted(rows, key=lambda r: (r[0] is None, r[0])):
        print("%-6s %-42s %s" % (ident if ident else "-", label, rel))
    print("\n%d faces" % len([r for r in rows if r[0]]))


if __name__ == "__main__":
    main(sys.argv[1] if len(sys.argv) > 1 else ".")

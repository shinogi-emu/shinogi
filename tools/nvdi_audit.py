"""Map every gemlib NVDI binding (n_*.c) to fVDI's dispatch.

Usage: python3 -I nvdi_audit.py <gemlib dir> <fvdi engine dir>
Prints: name, opcode, subop, fVDI handler, status.
"""
import os
import re
import sys

gem, eng = sys.argv[1], sys.argv[2]
tt = open(os.path.join(eng, "traptabl.s")).read().splitlines()


def table_after(label, sub=False):
    """Handlers in order after a label: main table entries are
    'dc.w a,b' + 'dc.l h' or 'dc.l 0,h'; escape tables are 'dc.l h'."""
    start = next(i for i, l in enumerate(tt) if l.strip() == label + ":")
    out = []
    for l in tt[start + 1:]:
        s = l.split(";")[0].strip()
        if not s or s.startswith("*"):
            continue
        if s.endswith(":") or s.startswith("dc.w") and sub:
            if s.endswith(":"):
                break
            continue
        m = re.match(r"dc\.l\s+(?:0,\s*)?([A-Za-z_][\w]*)", s)
        if m:
            out.append(m.group(1))
        elif s.startswith("dc.w"):
            continue
        else:
            break
    return out


main = table_after("default_functions")
op5 = table_after("default_opcode5", sub=True)
op11 = table_after("default_opcode11", sub=True)

# Labels inside start_unimpl..end_unimpl are stubs.
un = open(os.path.join(eng, "unimpl.s")).read()
body = un[un.index("start_unimpl:"):un.index("end_unimpl:")]
stubs = set(re.findall(r"^([A-Za-z_]\w*):", body, re.M))

rows = []
for fn in sorted(os.listdir(gem)):
    if not (fn.startswith("n_") and fn.endswith(".c")):
        continue
    src = open(os.path.join(gem, fn)).read()
    calls = re.findall(r"VDI_TRAP(_ESC|_00)?\s*\(\s*[^,]+,\s*[^,]+,\s*(\d+)\s*(?:,\s*(\d+))?", src)
    if not calls:
        rows.append((fn[2:-2], "-", "-", "-", "no direct trap (wrapper)"))
        continue
    for kind, op, sub in calls:
        op = int(op)
        subop = int(sub) if kind == "_ESC" and sub else None
        if op == 5 and subop is not None:
            h = op5[subop] if subop < len(op5) else None
        elif op == 11 and subop is not None:
            h = op11[subop] if subop < len(op11) else None
        else:
            h = main[op] if op < len(main) else None
        if h is None:
            st = "MISSING (opcode beyond table)"
        elif h == "nothing":
            st = "MISSING (empty slot)"
        elif h in stubs:
            st = "STUB (unimpl.s)"
        elif subop is not None and op not in (5, 11):
            st = "handler exists; sub-function to check"
        else:
            st = "implemented"
        rows.append((fn[2:-2], op, "" if subop is None else subop, h or "-", st))

seen = set()
for r in rows:
    if r in seen:
        continue
    seen.add(r)
    print("%-26s %4s %4s  %-20s %s" % r)

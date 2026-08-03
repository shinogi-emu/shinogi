#!/usr/bin/env python3
#
# Rebuild the MANY/ fixture directory that tests/golden/phase5-many,
# phase5-listing and phase5-dta assert against.
#
# 120 files is deliberately more than one 9P Rreaddir reply holds
# (about thirty at the guest's 1KB buffer), so a directory read that
# does not page cannot pass. The four name shapes cover an upper-case
# stem, a lower-case stem, a name with no extension and a stem longer
# than 8.3 allows; in raw byte order they group N < Z < a < d, which is
# what fixes the first and last names the golden pins down.
#
# Nothing else in the folder is touched -- the other goldens depend on
# the files already there.
#
# Usage: make-many-fixture.py [dir ...]   (default: both fixture folders)

import os
import sys

DEFAULTS = ["/tmp/shinogi-hostfs", os.path.expanduser("~/shinogi-drive-c")]


def name(i):
    return ["data%03d.bin", "Note%03d.txt", "ZZ%03d.doc", "a%03d"][i % 4] % i


for folder in (sys.argv[1:] or DEFAULTS):
    many = os.path.join(folder, "MANY")
    os.makedirs(many, exist_ok=True)
    for i in range(120):
        with open(os.path.join(many, name(i)), "w") as f:
            f.write("entry %d\n" % i)
    print("%s: %d entries" % (many, len(os.listdir(many))))

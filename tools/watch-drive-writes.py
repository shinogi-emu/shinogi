#!/usr/bin/env python3
"""Watch what happens to the host folder behind a vvfat drive.

    tools/watch-drive-writes.py <folder> [--interval 0.5]

Snapshots the folder continuously and reports every change as it
happens: files appearing, vanishing, changing length, or changing
content at the same length. Run it in one terminal, the guest in
another, and the two can be read side by side.

Why this exists
---------------
QEMU's vvfat driver synthesises a FAT filesystem over a real directory.
Its read-write mode is documented as experimental, and testing found it
does two things silently:

  * a file the guest deletes can stay on the host, and
  * deleting a file and then writing a DIFFERENT one truncates the new
    file to the deleted one's length.

Neither produces an error anywhere. The guest is told the write
succeeded, and the damage is only visible on the host - which is what
this watches. A wrong-length file is the signature to look for.

Pair it with QEMU's own block tracing to see both halves:

    qemu-system-m68k ... \\
        -trace virtio_blk_handle_write -trace blk_co_pwritev -D writes.log

That shows the sectors the guest asked to write; this shows what
reached the disk.
"""

import hashlib
import os
import sys
import time


def say(msg):
    """Print and flush. Redirected output is block-buffered otherwise, so
    everything observed so far is lost if the watcher is interrupted --
    which is exactly when it is most wanted."""
    print(msg)
    sys.stdout.flush()


def snapshot(root):
    """name -> (size, sha256) for every file under root."""
    out = {}
    for dirpath, dirnames, filenames in os.walk(root):
        for name in filenames:
            full = os.path.join(dirpath, name)
            rel = os.path.relpath(full, root)
            try:
                with open(full, 'rb') as fh:
                    data = fh.read()
                out[rel] = (len(data), hashlib.sha256(data).hexdigest())
            except OSError as e:
                out[rel] = (-1, 'unreadable: %s' % e)
        for name in dirnames:
            rel = os.path.relpath(os.path.join(dirpath, name), root)
            out[rel + '/'] = (0, 'dir')
    return out


def report(before, after):
    """Print what changed. Returns True if anything did."""
    stamp = time.strftime('%H:%M:%S')
    changed = False

    for name in sorted(set(after) - set(before)):
        size, _ = after[name]
        say('%s  +  %-40s %s' % (stamp, name, 'dir' if name.endswith('/')
                                 else '%d bytes' % size))
        changed = True

    for name in sorted(set(before) - set(after)):
        say('%s  -  %-40s removed' % (stamp, name))
        changed = True

    for name in sorted(set(before) & set(after)):
        (osize, osum), (nsize, nsum) = before[name], after[name]
        if osize != nsize:
            # The signature of the vvfat truncation bug.
            flag = '  <-- TRUNCATED' if nsize < osize else ''
            say('%s  ~  %-40s %d -> %d bytes%s'
                % (stamp, name, osize, nsize, flag))
            changed = True
        elif osum != nsum:
            say('%s  ~  %-40s same length, different content'
                % (stamp, name))
            changed = True

    return changed


def main():
    if len(sys.argv) < 2:
        print(__doc__)
        return 2

    root = sys.argv[1]
    interval = 0.5
    if '--interval' in sys.argv:
        interval = float(sys.argv[sys.argv.index('--interval') + 1])

    if not os.path.isdir(root):
        print('not a directory: %s' % root, file=sys.stderr)
        return 2

    before = snapshot(root)
    say('watching %s  (%d files)   Ctrl-C to stop'
        % (root, len([k for k in before if not k.endswith('/')])))
    say('%s     baseline taken' % time.strftime('%H:%M:%S'))

    try:
        while True:
            time.sleep(interval)
            after = snapshot(root)
            if report(before, after):
                before = after
    except KeyboardInterrupt:
        print('\nstopped.')
        return 0


if __name__ == '__main__':
    sys.exit(main())

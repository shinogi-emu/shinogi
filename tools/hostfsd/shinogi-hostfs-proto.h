/*
 * shinogi-hostfs-proto.h - the wire between the guest and the host helper
 *
 * Shared verbatim by both ends: the host helper (tools/hostfsd) and the
 * guest transport (bios/hostfs_link.c). It holds nothing but constants
 * and the frame layout, so it compiles inside EmuTOS as happily as it
 * does against a host libc -- no types, no includes, no endian helpers.
 *
 * Design: docs/hostfs-link-design.md.
 *
 * Framing
 * -------
 *   request   u16 len      total bytes INCLUDING this 16-byte header
 *             u16 op
 *             u32 a, b, c  operation-dependent
 *             u8  data[len - 16]
 *
 *   reply     u16 len      total bytes INCLUDING this 12-byte header
 *             u16 status   0 = ok, else a GEMDOS error (negative)
 *             u32 a, b     operation-dependent
 *             u8  data[len - 12]
 *
 * Every field is little-endian, matching virtio and the guest's existing
 * helpers. One request, one reply, strictly alternating: the guest is
 * single-threaded through osif() and cannot have two calls outstanding,
 * so a request id would be dead weight.
 *
 * `status` is always a GEMDOS error, never an errno. The helper owns the
 * mapping so a host error code can never reach the guest, the same
 * discipline p9_errno_to_gemdos() established.
 *
 * Path rules on the wire
 * ----------------------
 * A path is drive-relative, uses '/' as its ONLY separator, and never
 * begins with one. The guest transport converts from the Atari '\'
 * spelling and reduces "." and ".." itself, exactly as hostfs.c already
 * does before any request is built. The helper therefore refuses '\',
 * a leading '/', ':' and every "." or ".." component outright -- on the
 * wire those can only be an escape attempt, because a correct guest
 * never emits them.
 */

#ifndef SHINOGI_HOSTFS_PROTO_H
#define SHINOGI_HOSTFS_PROTO_H

/* Bumped whenever the meaning of any field changes. HELLO refuses a
 * mismatch rather than guessing, because a half-understood frame is
 * indistinguishable from a corrupted one. */
#define HOSTFS_PROTO_VERSION    1

#define HOSTFS_REQ_HDR          16
#define HOSTFS_REP_HDR          12

/*
 * The largest frame either side sends or accepts in a session that has
 * not raised it with CAPS (HOSTFS_CAP_LARGE_FRAME below) -- which is
 * every session to begin with, and every session of a guest that never
 * asks. A guest that never asks can size its buffers from this and need
 * never see a larger frame in either direction. Both ends reject a `len`
 * larger than the session's limit before a single field is read -- a
 * length from the peer is never trusted.
 */
#define HOSTFS_MAX_FRAME        4096
#define HOSTFS_MAX_REQ_DATA     (HOSTFS_MAX_FRAME - HOSTFS_REQ_HDR)
#define HOSTFS_MAX_REP_DATA     (HOSTFS_MAX_FRAME - HOSTFS_REP_HDR)

/* The most CAPS can raise it to: `len` is a u16. */
#define HOSTFS_MAX_FRAME_LIMIT  65535

/* Longest single path a request may carry, and longest entry name a
 * READDIR may return. Both well inside HOSTFS_MAX_REP_DATA so a name
 * can never be the thing that overflows a frame. */
#define HOSTFS_MAX_PATH         1024
#define HOSTFS_MAX_NAME         255

/* Operations. See the table in docs/hostfs-link-design.md. */
#define HOSTFS_OP_HELLO         1
#define HOSTFS_OP_OPEN          2
#define HOSTFS_OP_CLOSE         3
#define HOSTFS_OP_READ          4
#define HOSTFS_OP_WRITE         5
#define HOSTFS_OP_STAT          6
#define HOSTFS_OP_OPENDIR       7
#define HOSTFS_OP_READDIR       8
#define HOSTFS_OP_CLOSEDIR      9
#define HOSTFS_OP_CREATE        10
#define HOSTFS_OP_DELETE        11
#define HOSTFS_OP_RENAME        12
#define HOSTFS_OP_MKDIR         13
#define HOSTFS_OP_RMDIR         14
#define HOSTFS_OP_SETATTR       15
#define HOSTFS_OP_DFREE         16
#define HOSTFS_OP_CAPS          17
#define HOSTFS_OP_READDIR_BATCH 18
#define HOSTFS_OP_SYNC          19
#define HOSTFS_OP_MAX           19

/* Optional v1 operations. Old helpers reject CAPS; guests then use READDIR. */
#define HOSTFS_CAP_READDIR_BATCH 1UL

/*
 * Frames larger than HOSTFS_MAX_FRAME, by agreement. A guest that wants
 * them sends CAPS with `a` = the largest frame it can receive (and is
 * prepared to send); the helper answers with this bit set and `b` = the
 * frame limit for the rest of the session, in both directions:
 * min(a, the helper's own limit), never below HOSTFS_MAX_FRAME. An `a`
 * of HOSTFS_MAX_FRAME or less -- every guest written before this bit --
 * keeps the session at HOSTFS_MAX_FRAME, and HELLO always puts it back
 * there, because a guest that rebooted may be an older one.
 *
 * What changes inside a raised session: a request may be up to `b` bytes
 * long, and READ returns up to `b` - HOSTFS_REP_HDR bytes if asked for
 * that many. Nothing else grows: READDIR_BATCH still fills at most
 * HOSTFS_MAX_REP_DATA, so a guest's batch buffer needs no resizing.
 * A helper without the bit leaves `b` at 0 and keeps 4 KB frames.
 */
#define HOSTFS_CAP_LARGE_FRAME   2UL

/*
 * SYNC: the guest's Sync(), Fsync() and shutdown, carried to the host.
 * Writes reach the host's cache as they are made and CLOSE does not
 * flush, so this is how a guest asks for its data to be on the host's
 * stable storage.
 *
 *   a = 0  posted: the reply comes at once, and the helper flushes what
 *          the session wrote once no request has come for 20 ms, one file
 *          at a time; a request that arrives meanwhile waits at most for
 *          the one flush in progress. Under traffic that never pauses,
 *          once it has waited 5 s, it flushes one file, serves 100 ms of
 *          requests, flushes the next, and so on; a request still waits
 *          at most for one flush. Posting again before that is done adds
 *          nothing.
 *   a = 1  wait: when the reply arrives, everything the session wrote
 *          before the request is on stable storage -- files still open,
 *          files closed since, the names it created, renamed or deleted,
 *          and whatever an earlier a = 0 had not finished. Status 0, or
 *          an error if any of it could not be flushed: HOSTFS_ENOSPC
 *          when the host said its disk is full, HOSTFS_EIO for any other
 *          failure, and ENOSPC when there are both. The error reply is
 *          the 12-byte header alone. A file that failed is tried again,
 *          and reported again, by every later a = 1 until it flushes, is
 *          deleted or is renamed to something that flushes. A failure
 *          the helper met before the a = 1 -- in posted work, or in a
 *          flush it made in place -- is reported by the next a = 1 of
 *          the same session, once: on Linux a failed writeback is
 *          reported to the first flush only, so a retry would pass with
 *          nothing written. A HELLO starts a new session. The helper
 *          starts no new flush 240 s after the request arrived, inside
 *          the guest's 300 s for the reply: it then answers EIO and
 *          leaves the rest to be flushed as if posted.
 *
 * b, c and the data must be empty; anything else is refused (EDRIVE).
 * A guest checks this bit first: older helpers answer SYNC with EDRIVE.
 * The helper does the same flush, as for a = 1, when a session ends.
 *
 * On Windows a flush needs a handle that may write. A file the guest
 * makes read-only -- SETATTR, or CREATE with HOSTFS_FA_RO -- is
 * therefore flushed before it becomes read-only (or, if it is open, when
 * it is closed), so its SYNC succeeds. A file the HOST makes read-only,
 * or that a host program holds open without sharing write access, can
 * still fail a SYNC there with EIO. A name that has meanwhile become
 * a folder, a pipe or anything but a regular file is dropped unflushed:
 * the file it named is gone.
 */
#define HOSTFS_CAP_SYNC          4UL

/*
 * Per-operation field use. `data` is the remainder of the frame; a path
 * is NOT NUL-terminated on the wire, its length is len - 16.
 *
 *  op        request                          reply
 *  --------  -------------------------------  ---------------------------
 *  HELLO     a = guest protocol version       a = host version
 *                                             b = max data bytes per frame
 *                                             (HOSTFS_MAX_REP_DATA; a
 *                                             larger frame needs CAPS)
 *                                             data = folder label
 *  OPEN      a = HOSTFS_O_*, data = path      a = handle
 *  CLOSE     a = handle                       --
 *  READ      a = handle, b = offset,          a = bytes read
 *            c = count                        data = the bytes
 *  WRITE     a = handle, b = offset,          a = bytes written
 *            data = the bytes
 *  STAT      data = path                      a = size, b = mtime,
 *                                             data[0] = attribute byte
 *  OPENDIR   data = path                      a = handle
 *  READDIR   a = handle, b = cookie           a = next cookie
 *            (b = 0 starts the enumeration)   data = entry name
 *  CLOSEDIR  a = handle                       --
 *  CREATE    a = attribute byte, data = path  a = handle
 *  DELETE    data = path                      --
 *  RENAME    data = old '\0' new              --
 *  MKDIR     data = path                      --
 *  RMDIR     data = path                      --
 *  SETATTR   a = attribute byte, data = path  --
 *  DFREE     --                               a = free KB, b = total KB
 *  CAPS      a = largest frame the guest      a = capability bits
 *            accepts (0: HOSTFS_MAX_FRAME)    b = frame limit from now on
 *  SYNC      a = 0 posted, 1 wait             --
 *  READDIR_BATCH a = handle, b = cookie       a = last cookie, b = count
 *                                             data = repeated little-endian
 *                                             u32 next cookie, u16 name length,
 *                                             then exactly name length bytes
 *
 * mtime is seconds since the Unix epoch, UTC. The guest converts; see
 * the note in hostfs.c on why UTC and not the host's local time.
 *
 * DFREE is in kilobytes rather than bytes so a folder on a large volume
 * does not wrap a u32, and clamped to HOSTFS_DFREE_MAX_KB so the guest's
 * cluster arithmetic cannot overflow either.
 */

/* OPEN modes, the low two bits of GEMDOS Fopen. */
#define HOSTFS_O_RDONLY         0
#define HOSTFS_O_WRONLY         1
#define HOSTFS_O_RDWR           2

/* GEMDOS attribute bits, as the guest's hostfs_attr() builds them. */
#define HOSTFS_FA_RO            0x01
#define HOSTFS_FA_HIDDEN        0x02
#define HOSTFS_FA_SYSTEM        0x04
#define HOSTFS_FA_VOLUME        0x08
#define HOSTFS_FA_SUBDIR        0x10
#define HOSTFS_FA_ARCHIVE       0x20

/* 2GB expressed in KB: GEMDOS Dfree hands the guest a cluster count that
 * must stay comfortably inside a signed 32-bit number after scaling. */
#define HOSTFS_DFREE_MAX_KB     0x00200000UL

/*
 * READDIR cookies.
 *
 * The cookie is a plain 32-bit index into a snapshot the helper takes at
 * OPENDIR time -- deliberately NOT the host's telldir() value. The 9p
 * transport passed telldir()'s opaque 64-bit cookie through and the
 * guest kept only its low half, which is harmless on tmpfs (small
 * integers) and fatal on ext4 (a hash whose identifying half lives in
 * the HIGH word): every seek landed back at the start and the directory
 * repeated its first page for ever. A 32-bit index has no half to drop.
 *
 * 0 starts an enumeration. The reply's `a` is the cookie for the NEXT
 * call. End of directory is reported as ENMFIL, not as an empty entry.
 *
 * The snapshot means an enumeration sees the directory as it was at
 * OPENDIR: a file created afterwards is not listed until the next
 * OPENDIR. A file DELETED afterwards is skipped, because the helper
 * re-checks each entry still exists before returning it -- a name the
 * guest cannot then stat is worse than a name missing from the list.
 * Entries are returned in strcmp order so a listing is reproducible
 * across hosts; readdir order itself is not.
 */

/* GEMDOS and MiNT errors, the only values `status` may carry besides 0. */
#define HOSTFS_E_OK             0
#define HOSTFS_EFILNF           (-33)
#define HOSTFS_EPTHNF           (-34)
#define HOSTFS_ENHNDL           (-35)
#define HOSTFS_EACCDN           (-36)
#define HOSTFS_EIHNDL           (-37)
#define HOSTFS_ENSMEM           (-39)
#define HOSTFS_EDRIVE           (-46)
#define HOSTFS_ENMFIL           (-49)
/* MiNT's I/O error and full disk, which only SYNC a = 1 answers with. */
#define HOSTFS_EIO              (-90)
#define HOSTFS_ENOSPC           (-91)

#endif /* SHINOGI_HOSTFS_PROTO_H */

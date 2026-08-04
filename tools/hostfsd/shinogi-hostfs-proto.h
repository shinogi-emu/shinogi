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
 * The largest frame either side will send or accept. Both ends size a
 * static buffer from this, and both reject a `len` larger than it before
 * a single field is read -- a length from the peer is never trusted.
 */
#define HOSTFS_MAX_FRAME        4096
#define HOSTFS_MAX_REQ_DATA     (HOSTFS_MAX_FRAME - HOSTFS_REQ_HDR)
#define HOSTFS_MAX_REP_DATA     (HOSTFS_MAX_FRAME - HOSTFS_REP_HDR)

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
#define HOSTFS_OP_MAX           16

/*
 * Per-operation field use. `data` is the remainder of the frame; a path
 * is NOT NUL-terminated on the wire, its length is len - 16.
 *
 *  op        request                          reply
 *  --------  -------------------------------  ---------------------------
 *  HELLO     a = guest protocol version       a = host version
 *                                             b = max data bytes per frame
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

/* GEMDOS errors, the only values `status` may carry besides 0. */
#define HOSTFS_E_OK             0
#define HOSTFS_EFILNF           (-33)
#define HOSTFS_EPTHNF           (-34)
#define HOSTFS_ENHNDL           (-35)
#define HOSTFS_EACCDN           (-36)
#define HOSTFS_EIHNDL           (-37)
#define HOSTFS_ENSMEM           (-39)
#define HOSTFS_EDRIVE           (-46)
#define HOSTFS_ENMFIL           (-49)

#endif /* SHINOGI_HOSTFS_PROTO_H */

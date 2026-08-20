/*
 * shinogi-monitor - send one command to a running guest's control sockets.
 *
 *   shinogi-monitor <socket> <command> [args...]      human monitor
 *   shinogi-monitor --qmp <socket> <json>             QMP
 *   shinogi-monitor --qmp-seq <socket> <ms> <json>... QMP, paced
 *
 * The launcher needs to talk to the monitor to take screenshots and to
 * inject keys, and the package has to work on a machine where nothing
 * has been installed. socat and netcat are the obvious tools and neither
 * can be relied on; python3 is usually there and is a heavy thing to
 * depend on for forty bytes of socket traffic. So this is built static
 * and carried in the bundle.
 *
 * The monitor is a line protocol with a "(qemu) " prompt. There is no
 * length framing and no end-of-reply marker beyond that prompt, so the
 * reply is read until the prompt comes back or the link goes idle -- the
 * idle timeout matters because some commands (screendump of a large
 * scanout) answer with nothing at all, and waiting for a prompt that has
 * already been consumed would hang the launcher.
 *
 * QMP mode exists because the human monitor cannot position a pointer.
 * Its mouse_move queues RELATIVE motion, and the guest's pointing device
 * is a tablet, which reports absolute position and has no use for a
 * delta -- so the pointer does not go where the caller asked, and
 * nothing reports an error. input-send-event carries absolute axes and
 * is only reachable over QMP, which is a line of JSON rather than a
 * line of text. The JSON is composed by the caller; this end only does
 * the handshake, the write, and picking the reply out of the events.
 *
 * --qmp-seq exists because some input has to be PACED. The guest samples
 * its mouse once a frame, so a press and a release delivered in the same
 * millisecond are a transition it never observes: a double click sent as
 * one batch selects an icon and does not open it, and a drag sent as two
 * endpoints moves nothing. Pacing it from the shell means one process
 * per event and a sleep whose resolution is not promised; doing it here
 * means one connection, and gaps that are actually the length asked for.
 */
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <sys/un.h>

#define IDLE_MS 2000
#define REPLY_MAX (1 << 20)

static int connect_unix(const char *path)
{
    struct sockaddr_un sa;
    int fd;

    if (strlen(path) >= sizeof(sa.sun_path)) {
        fprintf(stderr, "socket path too long: %s\n", path);
        return -1;
    }
    memset(&sa, 0, sizeof(sa));
    sa.sun_family = AF_UNIX;
    strcpy(sa.sun_path, path);

    fd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (fd < 0) {
        perror("socket");
        return -1;
    }
    if (connect(fd, (struct sockaddr *)&sa, sizeof(sa)) < 0) {
        fprintf(stderr, "cannot reach %s: %s\n", path, strerror(errno));
        fprintf(stderr, "is the guest running?  try: shinogi status\n");
        return -1;
    }
    return fd;
}

/* The reply is collected rather than streamed because the monitor echoes
 * what it was sent -- one character at a time, each followed by the
 * terminal control sequences that redraw the line -- and that echo has to
 * come off the front before a caller can use the answer. */
static char reply[REPLY_MAX];
static size_t reply_len;

static int drain(int fd, int idle_ms, int want_prompt, int keep)
{
    char buf[4096];
    size_t seen = 0;
    char tail[8];

    memset(tail, 0, sizeof(tail));
    for (;;) {
        struct timeval tv;
        fd_set r;
        ssize_t n;
        int rc;

        FD_ZERO(&r);
        FD_SET(fd, &r);
        tv.tv_sec = idle_ms / 1000;
        tv.tv_usec = (idle_ms % 1000) * 1000;
        rc = select(fd + 1, &r, NULL, NULL, &tv);
        if (rc < 0) {
            if (errno == EINTR)
                continue;
            return -1;
        }
        if (rc == 0)
            return 0;               /* idle: treat as end of reply */
        n = read(fd, buf, sizeof(buf));
        if (n <= 0)
            return n == 0 ? 0 : -1;
        if (keep) {
            size_t room = sizeof(reply) - reply_len;
            size_t take = (size_t)n < room ? (size_t)n : room;
            memcpy(reply + reply_len, buf, take);
            reply_len += take;
        }
        seen += (size_t)n;
        if (want_prompt) {
            /* Keep the last 7 bytes so a prompt split across two reads
             * is still recognised. */
            size_t keep = (size_t)n < 7 ? (size_t)n : 7;
            if (keep < 7)
                memmove(tail, tail + keep, 7 - keep);
            memcpy(tail + 7 - keep, buf + (size_t)n - keep, keep);
            tail[7] = '\0';
            if (strstr(tail, "(qemu) "))
                return (int)seen;
        }
    }
}

/*
 * Strip the echo and the prompt.
 *
 * What comes back is: the echo of the command, then a newline, then the
 * answer, then "(qemu) " again. The echo is dropped by cutting at the
 * first newline -- the command itself never contains one, because it was
 * assembled from argv. A command that produces no output at all leaves
 * nothing between the two, which is correct: screendump answers with
 * silence and the file is the evidence.
 */
static void print_reply(void)
{
    char *body = reply;
    size_t len = reply_len;
    char *nl;

    reply[reply_len < sizeof(reply) ? reply_len : sizeof(reply) - 1] = '\0';
    nl = memchr(body, '\n', len);
    if (nl) {
        len -= (size_t)(nl + 1 - body);
        body = nl + 1;
    }

    /* Trailing prompt, and the blank line the monitor puts before it. */
    while (len >= 7 && memcmp(body + len - 7, "(qemu) ", 7) == 0)
        len -= 7;
    while (len > 0 && (body[len - 1] == '\n' || body[len - 1] == '\r'))
        len--;

    if (len) {
        fwrite(body, 1, len, stdout);
        fputc('\n', stdout);
    }
}

/* ------------------------------------------------------------------ QMP */

/*
 * Read one \n-terminated line. QMP is strictly line-delimited in both
 * directions, which is the whole reason this needs no JSON parser: the
 * reply to a command is a line containing "return" or "error", and
 * anything else on the wire is an asynchronous event to be skipped.
 */
static int read_line(int fd, char *buf, size_t cap, int idle_ms)
{
    size_t len = 0;

    while (len + 1 < cap) {
        struct timeval tv;
        fd_set r;
        int rc;

        FD_ZERO(&r);
        FD_SET(fd, &r);
        tv.tv_sec = idle_ms / 1000;
        tv.tv_usec = (idle_ms % 1000) * 1000;
        rc = select(fd + 1, &r, NULL, NULL, &tv);
        if (rc < 0) {
            if (errno == EINTR)
                continue;
            return -1;
        }
        if (rc == 0)
            return -1;                      /* idle: no line coming */
        if (read(fd, buf + len, 1) != 1)
            return -1;
        if (buf[len] == '\n') {
            buf[len] = '\0';
            return (int)len;
        }
        len++;
    }
    return -1;
}

static int qmp_send(int fd, const char *json, char *reply, size_t cap)
{
    size_t len = strlen(json);

    if (write(fd, json, len) != (ssize_t)len ||
        write(fd, "\n", 1) != 1)
        return -1;

    /* Events and the reply share the stream, so read past anything that
     * is not an answer to this command. */
    for (;;) {
        if (read_line(fd, reply, cap, IDLE_MS) < 0)
            return -1;
        if (strstr(reply, "\"return\"") || strstr(reply, "\"error\""))
            return strstr(reply, "\"error\"") ? 1 : 0;
    }
}

static int qmp_main(const char *path, char **json, int count, int gap_ms)
{
    char line[8192];
    int fd, rc, i;

    fd = connect_unix(path);
    if (fd < 0)
        return 1;

    /* The greeting arrives unprompted; capabilities must be negotiated
     * before any other command is accepted. */
    if (read_line(fd, line, sizeof(line), 2000) < 0) {
        fprintf(stderr, "no QMP greeting from %s\n", path);
        return 1;
    }
    if (qmp_send(fd, "{\"execute\":\"qmp_capabilities\"}", line, sizeof(line)) != 0) {
        fprintf(stderr, "QMP handshake refused: %s\n", line);
        return 1;
    }

    for (i = 0; i < count; i++) {
        if (i && gap_ms > 0)
            usleep((useconds_t)gap_ms * 1000);
        rc = qmp_send(fd, json[i], line, sizeof(line));
        if (rc < 0) {
            fprintf(stderr, "no reply to: %s\n", json[i]);
            return 1;
        }
        if (rc == 1) {
            fprintf(stderr, "%s\n", line);
            return 1;
        }
        /* A bare {"return": {}} is the usual success and says nothing
         * worth printing; anything richer is the caller's answer. */
        if (strcmp(line, "{\"return\": {}}") != 0)
            printf("%s\n", line);
    }
    close(fd);
    return 0;
}

int main(int argc, char **argv)
{
    char cmd[4096];
    size_t used = 0;
    int fd, i;

    if (argc >= 4 && strcmp(argv[1], "--qmp") == 0)
        return qmp_main(argv[2], argv + 3, argc - 3, 0);
    if (argc >= 5 && strcmp(argv[1], "--qmp-seq") == 0)
        return qmp_main(argv[2], argv + 4, argc - 4, atoi(argv[3]));

    if (argc < 3) {
        fprintf(stderr, "usage: %s <monitor-socket> <command> [args...]\n", argv[0]);
        fprintf(stderr, "       %s --qmp <qmp-socket> <json>\n", argv[0]);
        fprintf(stderr, "       %s --qmp-seq <qmp-socket> <gap-ms> <json>...\n", argv[0]);
        return 2;
    }

    for (i = 2; i < argc; i++) {
        size_t len = strlen(argv[i]);
        if (used + len + 2 >= sizeof(cmd)) {
            fprintf(stderr, "command too long\n");
            return 2;
        }
        if (used)
            cmd[used++] = ' ';
        memcpy(cmd + used, argv[i], len);
        used += len;
    }
    cmd[used++] = '\n';

    fd = connect_unix(argv[1]);
    if (fd < 0)
        return 1;

    /* Swallow the banner and the first prompt, so the caller sees only
     * the answer to its own command. */
    drain(fd, 700, 1, 0);

    if (write(fd, cmd, used) != (ssize_t)used) {
        perror("write");
        return 1;
    }
    if (drain(fd, IDLE_MS, 1, 1) < 0) {
        perror("read");
        return 1;
    }
    print_reply();
    fflush(stdout);
    close(fd);
    return 0;
}

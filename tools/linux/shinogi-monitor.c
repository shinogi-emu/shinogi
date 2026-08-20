/*
 * shinogi-monitor - send one command to a running guest's QEMU monitor.
 *
 *   shinogi-monitor <socket> <command> [args...]
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

int main(int argc, char **argv)
{
    struct sockaddr_un sa;
    char cmd[4096];
    size_t used = 0;
    int fd, i;

    if (argc < 3) {
        fprintf(stderr, "usage: %s <monitor-socket> <command> [args...]\n",
                argv[0]);
        return 2;
    }
    if (strlen(argv[1]) >= sizeof(sa.sun_path)) {
        fprintf(stderr, "socket path too long: %s\n", argv[1]);
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

    memset(&sa, 0, sizeof(sa));
    sa.sun_family = AF_UNIX;
    strcpy(sa.sun_path, argv[1]);

    fd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (fd < 0) {
        perror("socket");
        return 1;
    }
    if (connect(fd, (struct sockaddr *)&sa, sizeof(sa)) < 0) {
        fprintf(stderr, "cannot reach the monitor at %s: %s\n",
                argv[1], strerror(errno));
        fprintf(stderr, "is the guest running?  try: shinogi status\n");
        return 1;
    }

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

/* Guest-side checks for real packet round trips and interface reopen. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/socket.h>
#include <sys/select.h>
#include <sys/time.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <errno.h>
#include <sys/ioctl.h>
#include <net/if.h>
#include <sockios.h>

#define ROUNDS 48
#define BULK (2L * 1024 * 1024)

static long elapsed(const struct timeval *start, const struct timeval *end)
{
    return (end->tv_sec - start->tv_sec) * 1000L +
           (end->tv_usec - start->tv_usec) / 1000L;
}

static int readable(int fd)
{
    fd_set set;
    struct timeval timeout = {2, 0};
    FD_ZERO(&set);
    FD_SET(fd, &set);
    return select(fd + 1, &set, NULL, NULL, &timeout) == 1;
}

static void stats(FILE *log)
{
    struct ifreq request;
    int fd = socket(AF_INET, SOCK_DGRAM, 0);
    memset(&request, 0, sizeof(request));
    strcpy(request.ifr_name, "eth0");
    if (fd >= 0 && ioctl(fd, SIOCGIFSTATS, &request) == 0)
        fprintf(log, "STATS rx=%lu rx_errors=%lu tx=%lu tx_errors=%lu\n",
                request.ifr_stats.in_packets, request.ifr_stats.in_errors,
                request.ifr_stats.out_packets, request.ifr_stats.out_errors);
    if (fd >= 0) close(fd);
}

int main(int argc, char **argv)
{
    FILE *log;
    struct sockaddr_in peer;
    struct timeval start, end;
    unsigned char sent[1024], got[4096];
    long total = 0, millis;
    int fd, i, n, failed = 0;
    if (argc != 4) return 2;
    log = fopen(argv[1], "w");
    if (!log) return 2;
    setvbuf(log, NULL, _IONBF, 0);
    fprintf(log, "NETPROBE BEGIN\n");
    stats(log);
    memset(&peer, 0, sizeof(peer));
    peer.sin_family = AF_INET;
    peer.sin_addr.s_addr = inet_addr("10.0.2.2");
    peer.sin_port = htons(atoi(argv[2]));
    fd = socket(AF_INET, SOCK_DGRAM, 0);
    if (fd < 0) { fprintf(log, "FAIL UDP socket\n"); fclose(log); return 1; }
    gettimeofday(&start, NULL);
    for (i = 0; i < ROUNDS; ++i) {
        int count, ready = 0, received = -1;
        memset(sent, i, sizeof(sent));
        count = sendto(fd, sent, sizeof(sent), 0, (struct sockaddr *)&peer, sizeof(peer));
        if (count == sizeof(sent)) ready = readable(fd);
        if (ready) received = recv(fd, got, sizeof(got), 0);
        if (count != sizeof(sent) || !ready || received != sizeof(sent) ||
            memcmp(sent, got, sizeof(sent))) {
            fprintf(log, "FAIL UDP round %d sent=%d ready=%d received=%d errno=%d\n",
                    i, count, ready, received, errno);
            failed = 1;
            break;
        }
    }
    gettimeofday(&end, NULL);
    millis = elapsed(&start, &end);
    fprintf(log, "UDP %d/%d elapsed_ms=%ld\n", i, ROUNDS, millis);
    close(fd);
    if (!failed) {
        peer.sin_port = htons(atoi(argv[3]));
        fd = socket(AF_INET, SOCK_STREAM, 0);
        gettimeofday(&start, NULL);
        if (fd < 0 || connect(fd, (struct sockaddr *)&peer, sizeof(peer))) {
            fprintf(log, "FAIL TCP connect\n");
            failed = 1;
        } else {
            while (total < BULK && readable(fd)) {
                n = recv(fd, got, sizeof(got), 0);
                if (n <= 0) break;
                for (i = 0; i < n; ++i)
                    if (got[i] != (unsigned char)(total + i)) failed = 1;
                total += n;
            }
            gettimeofday(&end, NULL);
            millis = elapsed(&start, &end);
            fprintf(log, "TCP bytes=%ld elapsed_ms=%ld content=%s\n",
                    total, millis, failed ? "FAIL" : "OK");
            if (total != BULK) failed = 1;
            if (!failed) {
                for (i = 0; i < (int)sizeof(got); ++i) got[i] = (unsigned char)i;
                total = 0;
                gettimeofday(&start, NULL);
                while (total < BULK) {
                    long offset = total % sizeof(got);
                    n = send(fd, got + offset, sizeof(got) - offset, 0);
                    if (n <= 0) break;
                    total += n;
                }
                gettimeofday(&end, NULL);
                millis = elapsed(&start, &end);
                if (total != BULK || !readable(fd) ||
                    recv(fd, sent, 4, MSG_WAITALL) != 4 || memcmp(sent, "OKAY", 4))
                    failed = 1;
                fprintf(log, "UPLOAD bytes=%ld elapsed_ms=%ld host_content=%s\n",
                        total, millis, failed ? "FAIL" : "OK");
            }
        }
        if (fd >= 0) close(fd);
    }
    fprintf(log, "NETPROBE %s\n", failed ? "FAIL" : "PASS");
    stats(log);
    fclose(log);
    return failed;
}

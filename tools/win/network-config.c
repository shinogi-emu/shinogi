#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "network-config.h"

static int fail(char *error, size_t size, const char *message)
{
    if (size) snprintf(error, size, "%s", message);
    return 0;
}

int network_ipv4(const char *text, unsigned long *value)
{
    unsigned long result = 0;
    int part;
    for (part = 0; part < 4; ++part) {
        unsigned long octet = 0;
        int digits = 0;
        while (*text >= '0' && *text <= '9') {
            octet = octet * 10 + (unsigned)(*text++ - '0');
            if (++digits > 3 || octet > 255) return 0;
        }
        if (!digits) return 0;
        result = (result << 8) | octet;
        if (part < 3 && *text++ != '.') return 0;
    }
    if (*text) return 0;
    *value = result;
    return 1;
}

int network_guid_valid(const char *text)
{
    size_t i;
    if (strlen(text) != 38 || text[0] != '{' || text[37] != '}') return 0;
    for (i = 1; i < 37; ++i) {
        if (i == 9 || i == 14 || i == 19 || i == 24) {
            if (text[i] != '-') return 0;
        } else if (!isxdigit((unsigned char)text[i])) return 0;
    }
    return 1;
}

static int unicast(unsigned long value)
{
    return value >= 0x01000000UL && value < 0xe0000000UL &&
           (value >> 24) != 127;
}

int network_config_valid(const NetworkSettings *s, char *error, size_t size)
{
    unsigned long ip, mask, gateway, dns, inverse;
    size_t i;
    if (strlen(s->mac) != 17)
        return fail(error, size, "The saved network hardware address is invalid.");
    for (i = 0; i < 17; ++i) {
        if ((i + 1) % 3 == 0 ? s->mac[i] != ':' : !isxdigit((unsigned char)s->mac[i]))
            return fail(error, size, "The saved network hardware address is invalid.");
    }
    if (strtoul(s->mac, NULL, 16) & 1UL)
        return fail(error, size, "The network hardware address must be a unicast address.");
    if (s->lan != 0 && s->lan != 1)
        return fail(error, size, "Choose Internet or LAN networking in Settings.");
    if (!s->lan) return 1;
    if (!network_guid_valid(s->ethernet_guid))
        return fail(error, size, "Choose a physical Ethernet adapter for LAN networking.");
    if (s->dhcp != 0 && s->dhcp != 1)
        return fail(error, size, "Choose automatic or static LAN addressing in Advanced settings.");
    if (s->dhcp) return 1;
    if (!network_ipv4(s->address, &ip) || !unicast(ip))
        return fail(error, size, "Enter an unused Atari IPv4 address on your LAN.");
    if (!network_ipv4(s->netmask, &mask))
        return fail(error, size, "Enter a valid LAN subnet mask.");
    inverse = (~mask) & 0xffffffffUL;
    if (!mask || inverse < 3 || (inverse & (inverse + 1)))
        return fail(error, size, "Use a contiguous LAN subnet mask, from /1 to /30.");
    if (!(ip & inverse) || (ip & inverse) == inverse)
        return fail(error, size, "The Atari address cannot be the network or broadcast address.");
    if (!network_ipv4(s->gateway, &gateway) || !unicast(gateway) || gateway == ip ||
        (gateway & mask) != (ip & mask) || !(gateway & inverse) ||
        (gateway & inverse) == inverse)
        return fail(error, size, "Enter the LAN router address on the Atari subnet.");
    if (!network_ipv4(s->dns, &dns) || !unicast(dns))
        return fail(error, size, "Enter a DNS server IPv4 address.");
    return 1;
}

int network_qemu_options(const NetworkSettings *s, const char *tap_name,
                         char *out, size_t size)
{
    char escaped[NETWORK_NAME_SIZE * 2];
    size_t i, n = 0;
    int count;
    if (s->lan) {
        if (!*tap_name || strlen(tap_name) >= NETWORK_NAME_SIZE) return 0;
        for (i = 0; tap_name[i]; ++i) {
            if (tap_name[i] == '"' || (unsigned char)tap_name[i] < 32) return 0;
            escaped[n++] = tap_name[i];
            if (tap_name[i] == ',') escaped[n++] = ',';
        }
        escaped[n] = '\0';
        count = snprintf(out, size, " -netdev \"tap,id=net0,ifname=%s\""
                         " -device virtio-net-device,netdev=net0,mac=%s", escaped, s->mac);
    } else {
        count = snprintf(out, size, " -netdev user,id=net0,ipv6=off"
                         " -device virtio-net-device,netdev=net0,mac=%s", s->mac);
    }
    return count >= 0 && (size_t)count < size;
}

int network_guest_script(const NetworkSettings *s, char *out, size_t size)
{
    if (s->lan && s->dhcp) {
        int count = snprintf(out, size,
            "# Shinogi network settings v1; written before the next boot.\n"
            ": > /ram/etc/resolv.conf\n"
            "echo 'timeout 12; retry 5; select-timeout 1; initial-interval 1;' > /ram/etc/shndhcp.cnf\n"
            "/bin/dhclient -1 -cf /ram/etc/shndhcp.cnf -sf /c/mint/1-19-cur/shndhcp.sh"
            " -lf /ram/shndhcp.lea -pf /ram/shndhcp.pid eth0\n");
        return count >= 0 && (size_t)count < size;
    }
    int n = snprintf(out, size,
        "# Shinogi network settings v1; written before the next boot.\n"
        "/bin/ifconfig eth0 addr %s netmask %s up || exit 1\n"
        "/bin/route add default eth0 gw %s || exit 1\n"
        "echo 'nameserver %s' > /ram/etc/resolv.conf\n",
        s->lan ? s->address : "10.0.2.15", s->lan ? s->netmask : "255.255.255.0",
        s->lan ? s->gateway : "10.0.2.2", s->lan ? s->dns : "10.0.2.3");
    return n >= 0 && (size_t)n < size;
}

static int line_equal(const char *line, size_t len, const char *expected)
{
    if (len && line[len - 1] == '\r') --len;
    return len == strlen(expected) && !memcmp(line, expected, len);
}

static int line_contains(const char *line, size_t len, const char *word)
{
    size_t n = strlen(word), i;
    for (i = 0; i + n <= len; ++i)
        if (!memcmp(line + i, word, n)) return 1;
    return 0;
}

int network_boot_hook(const char *input, char **output, char *error, size_t size)
{
    const char *ifconfig = "exec u:/bin/ifconfig eth0 addr 10.0.2.15 netmask 255.255.255.0 up";
    const char *route = "exec u:/bin/route add default eth0 gw 10.0.2.2";
    const char *line = input;
    size_t length = strlen(input), used = 0;
    int got_if = 0, got_route = 0, got_hook = 0;
    char *out = malloc(length + strlen(SHINOGI_NETWORK_HOOK) + 4);
    *output = NULL;
    if (!out) return fail(error, size, "Not enough memory to prepare the guest network configuration.");
    while (*line) {
        const char *end = strchr(line, '\n');
        size_t len = end ? (size_t)(end - line) : strlen(line);
        size_t bytes = len + (end != NULL);
        if (line_equal(line, len, ifconfig)) {
            ++got_if;
            used += (size_t)sprintf(out + used, "%s%s", SHINOGI_NETWORK_HOOK,
                                   len && line[len - 1] == '\r' ? "\r\n" : "\n");
        } else if (line_equal(line, len, route)) {
            ++got_route;
        } else {
            if (line_equal(line, len, SHINOGI_NETWORK_HOOK)) ++got_hook;
            else {
                size_t start = 0;
                while (start < len && isspace((unsigned char)line[start])) ++start;
                if (start < len && line[start] != '#' &&
                    ((line_contains(line, len, "ifconfig") && line_contains(line, len, "eth0")) ||
                     (line_contains(line, len, "route") && line_contains(line, len, "default")))) {
                    free(out);
                    return fail(error, size, "MINT.CNF contains custom network setup. It has not been changed.");
                }
            }
            memcpy(out + used, line, bytes);
            used += bytes;
        }
        line += bytes;
    }
    out[used] = '\0';
    if (!((got_if == 1 && got_route == 1 && !got_hook) ||
          (!got_if && !got_route && got_hook == 1))) {
        free(out);
        return fail(error, size, "MINT.CNF has custom or duplicate networking commands.\n"
                    "Keep a backup and replace its network setup with the Shinogi network hook.");
    }
    *output = out;
    return 1;
}

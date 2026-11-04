#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "../../tools/win/network-config.h"

static NetworkSettings configured(void)
{
    NetworkSettings s = {0};
    strcpy(s.mac, "52:54:00:ab:cd:ef");
    strcpy(s.ethernet_guid, "{11111111-2222-3333-4444-555555555555}");
    strcpy(s.address, "192.168.1.60");
    strcpy(s.netmask, "255.255.255.0");
    strcpy(s.gateway, "192.168.1.1");
    strcpy(s.dns, "192.168.1.1");
    return s;
}

int main(int argc, char **argv)
{
    NetworkSettings s = configured();
    char text[2048], error[512], *hook, *again;
    unsigned long ip;
    const char *legacy = "# keep user config\r\nGEM=c:/thing.app\r\n"
        "exec u:/bin/ifconfig eth0 addr 10.0.2.15 netmask 255.255.255.0 up\r\n"
        "exec u:/bin/route add default eth0 gw 10.0.2.2\r\n# keep this too\r\n";
    const char *expected = "# keep user config\r\nGEM=c:/thing.app\r\n"
        SHINOGI_NETWORK_HOOK "\r\n# keep this too\r\n";
    if (argc == 2 && !strcmp(argv[1], "--dhcp-script")) {
        s.lan = 1;
        s.dhcp = 1;
        assert(network_guest_script(&s, text, sizeof(text)));
        fputs(text, stdout);
        return 0;
    }
    assert(network_config_valid(&s, error, sizeof(error)));
    assert(network_qemu_options(&s, "", text, sizeof(text)));
    assert(strstr(text, "user,id=net0,ipv6=off"));
    assert(strstr(text, "mac=52:54:00:ab:cd:ef"));
    assert(network_guest_script(&s, text, sizeof(text)));
    assert(strstr(text, "10.0.2.15") && strstr(text, "nameserver 10.0.2.3"));
    s.lan = 1;
    assert(network_config_valid(&s, error, sizeof(error)));
    assert(network_qemu_options(&s, "Shinogi LAN, test", text, sizeof(text)));
    assert(strstr(text, "ifname=Shinogi LAN,, test"));
    assert(strstr(text, "mac=52:54:00:ab:cd:ef"));
    assert(!network_qemu_options(&s, "unsafe\"adapter", text, sizeof(text)));
    assert(!network_qemu_options(&s, "", text, sizeof(text)));
    assert(!network_qemu_options(&s, "Shinogi LAN", text, 8));
    assert(network_guest_script(&s, text, sizeof(text)));
    assert(strstr(text, "addr 192.168.1.60") && strstr(text, "nameserver 192.168.1.1"));
    strcpy(s.address, "192.168.1.255");
    assert(!network_config_valid(&s, error, sizeof(error)));
    strcpy(s.address, "192.168.1.60");
    strcpy(s.gateway, "192.168.2.1");
    assert(!network_config_valid(&s, error, sizeof(error)));
    strcpy(s.gateway, "192.168.1.1");
    strcpy(s.netmask, "255.0.255.0");
    assert(!network_config_valid(&s, error, sizeof(error)));
    s.dhcp = 1;
    assert(network_config_valid(&s, error, sizeof(error))); /* Static fields are ignored. */
    assert(network_guest_script(&s, text, sizeof(text)));
    assert(strstr(text, "/bin/dhclient -1") && strstr(text, "-sf /c/mint/1-19-cur/shndhcp.sh"));
    assert(!strstr(text, "192.168.1.60"));
    assert(!network_ipv4("1.2.3.256", &ip));
    assert(!network_ipv4("1.2.3.4;exit", &ip));
    assert(!network_ipv4("1.2.3", &ip));
    assert(network_ipv4("192.168.1.60", &ip) && ip == 0xc0a8013cUL);
    assert(!network_guid_valid("{not-a-guid}"));
    assert(network_boot_hook(legacy, &hook, error, sizeof(error)));
    assert(!strcmp(hook, expected));
    assert(network_boot_hook(hook, &again, error, sizeof(error)));
    assert(!strcmp(again, hook));
    free(again); free(hook);
    assert(!network_boot_hook("exec u:/bin/ifconfig eth0 addr 192.168.1.5 up\n", &hook, error, sizeof(error)));
    assert(!network_boot_hook(SHINOGI_NETWORK_HOOK "\n" SHINOGI_NETWORK_HOOK "\n", &hook, error, sizeof(error)));
    assert(!network_boot_hook(SHINOGI_NETWORK_HOOK "\nexec u:/bin/route add default eth0 gw 1.2.3.4\n", &hook, error, sizeof(error)));
    puts("Network settings: NAT/LAN options, stable MAC, DHCP/static validation and boot-hook preservation passed");
    return 0;
}

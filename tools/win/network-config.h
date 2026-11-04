#ifndef SHINOGI_NETWORK_CONFIG_H
#define SHINOGI_NETWORK_CONFIG_H

#include <stddef.h>

#define NETWORK_GUID_SIZE 40
#define NETWORK_NAME_SIZE 256
#define SHINOGI_NETWORK_HOOK "exec u:/bin/bash u:/c/mint/1-19-cur/shnnet.sh"

typedef struct {
    int lan;
    int dhcp;
    char ethernet_guid[NETWORK_GUID_SIZE];
    char tap_guid[NETWORK_GUID_SIZE];
    char address[16], netmask[16], gateway[16], dns[16];
    char mac[18];
} NetworkSettings;

int network_ipv4(const char *text, unsigned long *value);
int network_guid_valid(const char *text);
int network_config_valid(const NetworkSettings *settings, char *error, size_t size);
int network_qemu_options(const NetworkSettings *settings, const char *tap_name,
                         char *out, size_t size);
int network_guest_script(const NetworkSettings *settings, char *out, size_t size);
/* Allocates a complete replacement; caller frees it. Refuses custom boot
 * networking rather than adding a second, conflicting interface setup. */
int network_boot_hook(const char *input, char **output, char *error, size_t size);

#endif

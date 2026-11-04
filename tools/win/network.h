#ifndef SHINOGI_WINDOWS_NETWORK_H
#define SHINOGI_WINDOWS_NETWORK_H

#include <windows.h>
#include "network-config.h"

typedef struct {
    char guid[NETWORK_GUID_SIZE], name[NETWORK_NAME_SIZE];
    int ethernet, dhcp, up, tap;
} NetworkAdapter;

int network_load(NetworkSettings *settings, const char *file, char *error, size_t size);
int network_save(const NetworkSettings *settings, const char *file);
int network_adapters(NetworkAdapter *adapters, size_t capacity);
int network_has_setup(void);
int network_setup_dialog(HWND owner, const char *ethernet_guid, int remove,
                         char *tap_guid, char *error, size_t size);
int network_prepare(const NetworkSettings *settings, const char *drivec,
                    char *options, size_t options_size, char *error, size_t size);

#endif

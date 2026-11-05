#ifndef SHINOGI_SETTINGS_H
#define SHINOGI_SETTINGS_H

#define IDD_SETTINGS 100
#define IDC_DRIVEC 101
#define IDC_BROWSE 102
#define IDC_RESOLUTION 103
#define IDC_FULLSCREEN 104
#define IDC_NETWORK_MODE 105
#define IDC_ETHERNET 106
#define IDC_IP_ADDRESS 107
#define IDC_NETMASK 108
#define IDC_GATEWAY 109
#define IDC_DNS 110
#define IDC_LAN_SETUP 111
#define IDC_LAN_REMOVE 112
#define IDC_LAN_ADVANCED 113
#define IDD_LAN_ADVANCED 120
#define IDC_DHCP 121
#define IDC_ADDRESS_MODE_LABEL 122
#define IDC_LAST_LAN_ADDRESS 123

#ifndef RC_INVOKED
#include <windows.h>
#include "network-config.h"
#include "drives.h"


typedef struct {
    char file[MAX_PATH];
    char drivec[MAX_PATH];
    int width, height, fullscreen;
    NetworkSettings network;
    DriveSettings drives;
} Settings;

int settings_load(Settings *settings, const char *program_dir);
void settings_resolution(Settings *settings);
int settings_dialog(HINSTANCE instance, Settings *settings);
int settings_drive_valid(const char *path);
#endif
#endif

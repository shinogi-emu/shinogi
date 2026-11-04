#include "window-controls.h"
#include <stdio.h>

#define SETTINGS_HOTKEY 0x5348

typedef struct {
    DWORD qemu_pid;
    int registered;
    int warned;
    const char *launcher;
    HANDLE settings;
    DWORD settings_pid;
} WindowControls;

/* WinEvent callbacks run on the thread pumping messages below. */
static WindowControls *current;

static DWORD foreground_pid(void)
{
    DWORD pid = 0;
    HWND window = GetForegroundWindow();
    if (window) GetWindowThreadProcessId(window, &pid);
    return pid;
}

static void update_shortcut(WindowControls *controls)
{
    int focused = foreground_pid() == controls->qemu_pid;
    if (!focused && controls->registered) {
        UnregisterHotKey(NULL, SETTINGS_HOTKEY);
        controls->registered = 0;
    } else if (focused && !controls->registered) {
        controls->registered = RegisterHotKey(NULL, SETTINGS_HOTKEY,
                                               MOD_CONTROL | MOD_ALT | MOD_NOREPEAT, 'S');
        if (!controls->registered && !controls->warned) {
            controls->warned = 1;
            MessageBoxA(NULL,
                "Ctrl+Alt+S is already used by another application.\n"
                "Open Shinogi Settings from the Start menu instead.",
                "Shinogi Settings", MB_ICONINFORMATION);
        }
    }
}

static void CALLBACK foreground_changed(HWINEVENTHOOK hook, DWORD event,
                                        HWND window, LONG object, LONG child,
                                        DWORD thread, DWORD time)
{
    (void)hook; (void)event; (void)window; (void)object;
    (void)child; (void)thread; (void)time;
    if (current) update_shortcut(current);
}

static BOOL CALLBACK show_settings_window(HWND window, LPARAM data)
{
    DWORD pid = 0;
    WindowControls *controls = (WindowControls *)data;
    GetWindowThreadProcessId(window, &pid);
    if (pid == controls->settings_pid && IsWindowVisible(window) &&
        GetWindow(window, GW_OWNER) == NULL) {
        ShowWindow(window, SW_RESTORE);
        SetForegroundWindow(window);
        return FALSE;
    }
    return TRUE;
}

static void open_settings(WindowControls *controls)
{
    char command[MAX_PATH + 32];
    STARTUPINFOA startup;
    PROCESS_INFORMATION process;

    /* A queued shortcut can arrive just after focus moved away. */
    if (foreground_pid() != controls->qemu_pid) return;
    if (controls->settings) {
        if (WaitForSingleObject(controls->settings, 0) == WAIT_TIMEOUT) {
            EnumWindows(show_settings_window, (LPARAM)controls);
            return;
        }
        CloseHandle(controls->settings);
        controls->settings = NULL;
    }
    snprintf(command, sizeof(command), "\"%s\" --settings", controls->launcher);
    ZeroMemory(&startup, sizeof(startup));
    startup.cb = sizeof(startup);
    ZeroMemory(&process, sizeof(process));
    if (!CreateProcessA(controls->launcher, command, NULL, NULL, FALSE,
                        0, NULL, NULL, &startup, &process)) {
        MessageBoxA(NULL, "Could not open Shinogi Settings.\n"
                    "Try the Settings shortcut in the Start menu.",
                    "Shinogi Settings", MB_ICONWARNING);
        return;
    }
    controls->settings = process.hProcess;
    controls->settings_pid = process.dwProcessId;
    CloseHandle(process.hThread);
}

void window_controls_wait(HANDLE process, DWORD pid, const char *launcher)
{
    WindowControls controls = {0};
    HWINEVENTHOOK hook;
    MSG message;
    DWORD result;

    controls.qemu_pid = pid;
    controls.launcher = launcher;
    /* Create the thread message queue before registering callbacks. */
    PeekMessageA(&message, NULL, 0, 0, PM_NOREMOVE);
    current = &controls;
    hook = SetWinEventHook(EVENT_SYSTEM_FOREGROUND, EVENT_SYSTEM_FOREGROUND,
                          NULL, foreground_changed, 0, 0, WINEVENT_OUTOFCONTEXT);
    if (!hook) {
        current = NULL;
        MessageBoxA(NULL, "The Settings shortcut could not be enabled.\n"
                    "Open Shinogi Settings from the Start menu instead.",
                    "Shinogi Settings", MB_ICONINFORMATION);
        WaitForSingleObject(process, INFINITE);
        return;
    }
    update_shortcut(&controls);
    for (;;) {
        result = MsgWaitForMultipleObjects(1, &process, FALSE, INFINITE, QS_ALLINPUT);
        if (result == WAIT_OBJECT_0) break;
        if (result == WAIT_FAILED) {
            WaitForSingleObject(process, INFINITE);
            break;
        }
        while (PeekMessageA(&message, NULL, 0, 0, PM_REMOVE)) {
            if (message.message == WM_HOTKEY && message.wParam == SETTINGS_HOTKEY)
                open_settings(&controls);
            else {
                TranslateMessage(&message);
                DispatchMessageA(&message);
            }
        }
    }
    if (controls.registered) UnregisterHotKey(NULL, SETTINGS_HOTKEY);
    UnhookWinEvent(hook);
    current = NULL;
    if (controls.settings) CloseHandle(controls.settings);
}

/* Exercise the real Windows shortcut with separate foreground processes. */
#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "../../tools/win/window-controls.h"

static char executable[MAX_PATH], directory[MAX_PATH];
static HANDLE guest_process, other_process;
static volatile LONG failed;

static void mark(const char *name)
{
    char path[MAX_PATH + 32];
    FILE *file;
    snprintf(path, sizeof(path), "%s\\%s", directory, name);
    file = fopen(path, "ab");
    if (file) { fputc('1', file); fclose(file); }
}

static long count(const char *name)
{
    char path[MAX_PATH + 32];
    FILE *file;
    long n = 0;
    snprintf(path, sizeof(path), "%s\\%s", directory, name);
    file = fopen(path, "rb");
    if (file) { fseek(file, 0, SEEK_END); n = ftell(file); fclose(file); }
    return n;
}

static LRESULT CALLBACK window_proc(HWND window, UINT message, WPARAM wp, LPARAM lp)
{
    if (message == WM_APP) {
        /* The foreground process grants this synthetic user focus change. */
        AllowSetForegroundWindow(ASFW_ANY);
        SetForegroundWindow((HWND)wp);
        return 0;
    }
    if (message == WM_KEYDOWN && wp == 'S') {
        char title[80];
        GetWindowTextA(window, title, sizeof(title));
        if (!strcmp(title, "Shortcut other")) mark("other-keys.txt");
    }
    if (message == WM_DESTROY) { PostQuitMessage(0); return 0; }
    return DefWindowProcA(window, message, wp, lp);
}

static int fixture(const char *title)
{
    WNDCLASSA cls = {0};
    HWND window;
    MSG message;
    mark("fixture-starts.txt");
    cls.lpfnWndProc = window_proc;
    cls.hInstance = GetModuleHandleA(NULL);
    cls.lpszClassName = "ShortcutFixture";
    RegisterClassA(&cls);
    window = CreateWindowA(cls.lpszClassName, title, WS_OVERLAPPEDWINDOW,
                            20, 20, 400, 200, NULL, NULL, cls.hInstance, NULL);
    if (!window) { mark("fixture-window-failures.txt"); return 2; }
    mark("fixture-windows.txt");
    AllowSetForegroundWindow(ASFW_ANY);
    ShowWindow(window, SW_SHOW);
    if (!strcmp(title, "Shortcut settings")) mark("settings-opens.txt");
    while (GetMessageA(&message, NULL, 0, 0) > 0) {
        TranslateMessage(&message);
        DispatchMessageA(&message);
    }
    return 0;
}

static PROCESS_INFORMATION start(const char *argument)
{
    char command[MAX_PATH + 40];
    STARTUPINFOA startup = {0};
    PROCESS_INFORMATION process = {0};
    startup.cb = sizeof(startup);
    snprintf(command, sizeof(command), "\"%s\" %s", executable, argument);
    if (!CreateProcessA(executable, command, NULL, NULL, FALSE, 0, NULL,
                        NULL, &startup, &process)) exit(2);
    CloseHandle(process.hThread);
    return process;
}

static HWND find(const char *title)
{
    unsigned i;
    HWND window;
    for (i = 0; i < 100; i++) {
        window = FindWindowA(NULL, title);
        if (window) return window;
        Sleep(50);
    }
    failed = 1;
    mark("find-failures.txt");
    return NULL;
}

static void shortcut(HWND window)
{
    INPUT input[6] = {0};
    unsigned i;
    if (GetForegroundWindow())
        SendMessageA(GetForegroundWindow(), WM_APP, (WPARAM)window, 0);
    else
        SetForegroundWindow(window);
    Sleep(250);
    if (GetForegroundWindow() != window) { failed = 1; mark("focus-failures.txt"); return; }
    for (i = 0; i < 6; i++) input[i].type = INPUT_KEYBOARD;
    input[0].ki.wVk = VK_CONTROL;
    input[1].ki.wVk = VK_MENU;
    input[2].ki.wVk = 'S';
    input[3].ki.wVk = 'S'; input[3].ki.dwFlags = KEYEVENTF_KEYUP;
    input[4].ki.wVk = VK_MENU; input[4].ki.dwFlags = KEYEVENTF_KEYUP;
    input[5].ki.wVk = VK_CONTROL; input[5].ki.dwFlags = KEYEVENTF_KEYUP;
    if (SendInput(6, input, sizeof(INPUT)) != 6) failed = 1;
    Sleep(500);
}

static DWORD WINAPI drive_test(void *unused)
{
    HWND guest = find("Shortcut guest"), other = find("Shortcut other");
    HWND settings;
    (void)unused;
    shortcut(other);
    if (count("settings-opens.txt") != 0 || count("other-keys.txt") != 1) failed = 1;
    shortcut(guest);
    settings = find("Shortcut settings");
    if (count("settings-opens.txt") != 1) failed = 1;
    shortcut(guest);
    if (count("settings-opens.txt") != 1) failed = 1;
    if (settings) PostMessageA(settings, WM_CLOSE, 0, 0);
    Sleep(300);
    shortcut(other);
    if (count("other-keys.txt") != 2) failed = 1;
    shortcut(guest);
    settings = find("Shortcut settings");
    if (count("settings-opens.txt") != 2) failed = 1;
    if (settings) PostMessageA(settings, WM_CLOSE, 0, 0);
    if (other) PostMessageA(other, WM_CLOSE, 0, 0);
    if (guest) PostMessageA(guest, WM_CLOSE, 0, 0);
    return 0;
}

int main(int argc, char **argv)
{
    PROCESS_INFORMATION guest, other;
    HANDLE thread;
    char *slash;
    GetModuleFileNameA(NULL, executable, sizeof(executable));
    strcpy(directory, executable);
    slash = strrchr(directory, '\\');
    if (!slash) return 2;
    *slash = '\0';
    if (argc > 1 && !strcmp(argv[1], "--guest")) return fixture("Shortcut guest");
    if (argc > 1 && !strcmp(argv[1], "--other")) return fixture("Shortcut other");
    if (argc > 1 && !strcmp(argv[1], "--settings")) return fixture("Shortcut settings");
    if (count("settings-opens.txt") || count("other-keys.txt")) return 2;
    AllowSetForegroundWindow(ASFW_ANY);
    guest = start("--guest"); other = start("--other");
    guest_process = guest.hProcess; other_process = other.hProcess;
    thread = CreateThread(NULL, 0, drive_test, NULL, 0, NULL);
    window_controls_wait(guest_process, guest.dwProcessId, executable);
    WaitForSingleObject(thread, 10000);
    WaitForSingleObject(other_process, 10000);
    CloseHandle(thread); CloseHandle(guest_process); CloseHandle(other_process);
    puts(failed ? "Window shortcut FAIL" : "Window shortcut: focus, repeat, reopen and unrelated keys passed");
    if (failed) printf("fixtures=%ld windows=%ld window_failures=%ld find_failures=%ld focus_failures=%ld settings=%ld other_keys=%ld\n",
        count("fixture-starts.txt"), count("fixture-windows.txt"), count("fixture-window-failures.txt"),
        count("find-failures.txt"), count("focus-failures.txt"), count("settings-opens.txt"), count("other-keys.txt"));
    return failed ? 1 : 0;
}

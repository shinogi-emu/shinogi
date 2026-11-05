/* Run only in a fresh test Windows profile with a disposable drive folder. */
#include <windows.h>
#include <shlobj.h>
#include <stdio.h>
#include <string.h>

static HWND emulator;
static const char *wanted;

static void report(const char *text)
{
    char self[MAX_PATH], path[MAX_PATH + 32];
    FILE *file;
    GetModuleFileNameA(NULL, self, sizeof(self));
    snprintf(path, sizeof(path), "%s-progress.txt", self);
    file = fopen(path, "a");
    if (file) { fprintf(file, "%s\n", text); fclose(file); }
    puts(text);
    fflush(stdout);
}

static BOOL CALLBACK find_emulator(HWND window, LPARAM unused)
{
    char title[512];
    (void)unused;
    GetWindowTextA(window, title, sizeof(title));
    if (IsWindowVisible(window) && strstr(title, wanted)) {
        emulator = window;
        return FALSE;
    }
    return TRUE;
}

int main(int argc, char **argv)
{
    char appdata[MAX_PATH], folder[MAX_PATH + 128], ini[MAX_PATH + 160];
    char command[MAX_PATH + 4], edition[80], settings_title[100];
    STARTUPINFOA startup = {0};
    PROCESS_INFORMATION launcher = {0};
    INPUT input[6] = {0};
    HWND dialog = NULL;
    DWORD pid, code;
    HANDLE process;
    unsigned i;
    if (argc < 4 || argc > 5) return 2;
    if (argc == 5 && strcmp(argv[4], "--no-shortcut")) return 2;
    report("Package test started");
    wanted = argv[3];
    snprintf(edition, sizeof(edition), "%s", argv[3]);
    edition[strcspn(edition, " ")] = 0;
    snprintf(settings_title, sizeof(settings_title), "%s Settings", edition);
    if (FAILED(SHGetFolderPathA(NULL, CSIDL_APPDATA | CSIDL_FLAG_CREATE, NULL, 0, appdata))) return 2;
    snprintf(folder, sizeof(folder), "%s\\%s", appdata, edition);
    snprintf(ini, sizeof(ini), "%s\\settings.ini", folder);
    if (GetFileAttributesA(ini) != INVALID_FILE_ATTRIBUTES) {
        puts("Use a fresh test profile; existing settings will not be changed.");
        return 2;
    }
    CreateDirectoryA(folder, NULL);
    if (!WritePrivateProfileStringA("Drives", "C", argv[2], ini)) return 2;
    report("Test profile configured");
    startup.cb = sizeof(startup);
    snprintf(command, sizeof(command), "\"%s\"", argv[1]);
    if (!CreateProcessA(argv[1], command, NULL, NULL, FALSE, 0, NULL, NULL, &startup, &launcher)) return 2;
    CloseHandle(launcher.hThread);
    report("Launcher started");
    for (i = 0; i < 200 && !emulator; i++) {
        EnumWindows(find_emulator, 0);
        Sleep(50);
    }
    if (!emulator) { puts("No SDL window"); return 1; }
    report("SDL window found");
    SetForegroundWindow(emulator);
    {
        RECT area;
        GetWindowRect(emulator, &area);
        SetCursorPos((area.left + area.right) / 2, (area.top + area.bottom) / 2);
    }
    Sleep(500);
    if (GetForegroundWindow() != emulator) { puts("SDL window did not acquire focus"); return 1; }
    GetWindowThreadProcessId(emulator, &pid);
    process = OpenProcess(SYNCHRONIZE | PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
    if (!process) return 2;
    if (argc == 4) {
        for (i = 0; i < 6; i++) input[i].type = INPUT_KEYBOARD;
        input[0].ki.wVk = VK_CONTROL; input[1].ki.wVk = VK_MENU; input[2].ki.wVk = 'S';
        input[3].ki.wVk = 'S'; input[3].ki.dwFlags = KEYEVENTF_KEYUP;
        input[4].ki.wVk = VK_MENU; input[4].ki.dwFlags = KEYEVENTF_KEYUP;
        input[5].ki.wVk = VK_CONTROL; input[5].ki.dwFlags = KEYEVENTF_KEYUP;
        if (SendInput(6, input, sizeof(INPUT)) != 6) return 2;
        report("Shortcut sent");
        for (i = 0; i < 200 && !dialog; i++) {
            dialog = FindWindowA("#32770", settings_title);
            if (dialog && !GetDlgItem(dialog, 101)) dialog = NULL;
            Sleep(50);
        }
        if (!dialog) { puts("Settings did not open from SDL"); return 1; }
        report("Real Settings dialog opened");
        PostMessageA(dialog, WM_COMMAND, IDCANCEL, 0);
        for (i = 0; i < 100 && IsWindow(dialog); i++) Sleep(50);
        if (IsWindow(dialog)) { puts("Settings did not close"); return 1; }
        report("Settings cancelled");
        if (!GetExitCodeProcess(process, &code) || code != STILL_ACTIVE) {
            puts("Opening Settings stopped QEMU"); return 1;
        }
        report("Settings shortcut passed; QEMU process remains active");
    } else {
        report("Shortcut skipped for comparison");
        Sleep(1000);
    }
    PostMessageA(emulator, WM_CLOSE, 0, 0);
    report("SDL close requested");
    if (WaitForSingleObject(launcher.hProcess, 15000) != WAIT_OBJECT_0) {
        char detail[128];
        GetExitCodeProcess(process, &code);
        snprintf(detail, sizeof(detail), "QEMU code=%lu window=%d", (unsigned long)code, IsWindow(emulator));
        report(detail);
        report("Launcher did not exit after SDL close"); return 1;
    }
    GetExitCodeProcess(launcher.hProcess, &code);
    CloseHandle(process); CloseHandle(launcher.hProcess);
    if (code) { printf("Launcher exited %lu\n", (unsigned long)code); return 1; }
    puts("Packaged SDL shortcut opened real Settings; emulator remained open; clean close passed");
    return 0;
}

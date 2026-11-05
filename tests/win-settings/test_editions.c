#define COBJMACROS
#include <windows.h>
#include <shlobj.h>
#include <assert.h>
#include <stdio.h>
#include <string.h>

static void execute(const char *file, const char *args)
{
    STARTUPINFOA si = {0};
    PROCESS_INFORMATION pi;
    char cmd[2048];
    DWORD code;
    si.cb = sizeof(si);
    snprintf(cmd, sizeof(cmd), "\"%s\" %s", file, args);
    assert(CreateProcessA(file, cmd, NULL, NULL, FALSE, 0, NULL, NULL, &si, &pi));
    CloseHandle(pi.hThread);
    assert(WaitForSingleObject(pi.hProcess, 180000) == WAIT_OBJECT_0);
    assert(GetExitCodeProcess(pi.hProcess, &code) && code == 0);
    CloseHandle(pi.hProcess);
}

static void installed(const char *edition, char *dir)
{
    HKEY key;
    DWORD n = MAX_PATH;
    char name[512];
    snprintf(name, sizeof(name), "Software\\shinogi\\Editions\\%s", edition);
    assert(RegOpenKeyExA(HKEY_LOCAL_MACHINE, name, 0, KEY_READ | KEY_WOW64_64KEY, &key) == ERROR_SUCCESS);
    assert(RegQueryValueExA(key, "InstallDir", NULL, NULL, (BYTE *)dir, &n) == ERROR_SUCCESS);
    RegCloseKey(key);
}

static void shortcut(const char *base, const char *edition, const char *target)
{
    char file[MAX_PATH], actual[MAX_PATH];
    WCHAR wide[MAX_PATH];
    IShellLinkA *link;
    IPersistFile *persist;
    snprintf(file, sizeof(file), "%s\\%s.lnk", base, edition);
    assert(MultiByteToWideChar(CP_ACP, 0, file, -1, wide, MAX_PATH));
    assert(SUCCEEDED(CoCreateInstance(&CLSID_ShellLink, NULL, CLSCTX_INPROC_SERVER, &IID_IShellLinkA, (void **)&link)));
    assert(SUCCEEDED(IShellLinkA_QueryInterface(link, &IID_IPersistFile, (void **)&persist)));
    assert(SUCCEEDED(IPersistFile_Load(persist, wide, STGM_READ)));
    assert(SUCCEEDED(IShellLinkA_GetPath(link, actual, sizeof(actual), NULL, SLGP_RAWPATH)));
    assert(!lstrcmpiA(actual, target));
    IPersistFile_Release(persist); IShellLinkA_Release(link);
}

int main(int argc, char **argv)
{
    char dir[3][MAX_PATH], appdata[MAX_PATH], desktop[MAX_PATH], programs[MAX_PATH], profile[MAX_PATH];
    PROCESS_INFORMATION running[3];
    char guest[3][MAX_PATH];
    int i;
    assert(argc == 10); /* three edition/installer pairs, guest probe, raw hard image, raw floppy */
    assert(SUCCEEDED(CoInitialize(NULL)));
    assert(SUCCEEDED(SHGetFolderPathA(NULL, CSIDL_APPDATA | CSIDL_FLAG_CREATE, NULL, 0, appdata)));
    assert(SUCCEEDED(SHGetFolderPathA(NULL, CSIDL_DESKTOPDIRECTORY | CSIDL_FLAG_CREATE, NULL, 0, desktop)));
    assert(SUCCEEDED(SHGetFolderPathA(NULL, CSIDL_PROGRAMS | CSIDL_FLAG_CREATE, NULL, 0, programs)));
    assert(SUCCEEDED(SHGetFolderPathA(NULL, CSIDL_PROFILE, NULL, 0, profile)));
    for (i = 0; i < 3; i++) {
        char ini[512], path[1024], expected[512], drive[MAX_PATH], menu[512], edition[80];
        const char *name = argv[i*2+1];
        execute(argv[i*2+2], "/S");
        installed(name, dir[i]);
        assert(strstr(dir[i], name));
        snprintf(ini, sizeof(ini), "%s\\%s\\settings.ini", appdata, name);
        assert(GetPrivateProfileStringA("Drives", "C", "", drive, sizeof(drive), ini));
        lstrcpynA(guest[i], drive, sizeof(guest[i]));
        snprintf(expected, sizeof(expected), "%s\\%s\\drive-c", profile, name);
        assert(!lstrcmpiA(expected, drive));
        snprintf(path, sizeof(path), "%s\\AUTO\\MINT.PRG", drive);
        assert(GetFileAttributesA(path) != INVALID_FILE_ATTRIBUTES);
        snprintf(path, sizeof(path), "%s\\edition.ini", dir[i]);
        assert(GetPrivateProfileStringA("Install", "Edition", "", edition, sizeof(edition), path));
        assert(!strcmp(edition, name));
        snprintf(path, sizeof(path), "%s\\shinogi.exe", dir[i]);
        shortcut(desktop, name, path);
        snprintf(menu, sizeof(menu), "%s\\%s", programs, name);
        shortcut(menu, name, path);
        printf("PASS installed %s: %s / %s\n", name, dir[i], drive); fflush(stdout);
    }
    for (i = 0; i < 3; i++) {
        char actual[MAX_PATH]; installed(argv[i*2+1], actual); assert(!strcmp(actual, dir[i]));
    }
    for (i = 0; i < 3; i++) {
        char folder[512], file[1024], ini[512], cmd[2048];
        STARTUPINFOA si = {0};
        FILE *f;
        snprintf(folder, sizeof(folder), "%s\\EXTRA", guest[i]); assert(CreateDirectoryA(folder, NULL));
        snprintf(file, sizeof(file), "%s\\EXTRA\\EXTRA.TXT", guest[i]);
        f = fopen(file, "wb"); assert(f); fputs("WINDOWS FOLDER\n", f); fclose(f);
        snprintf(file, sizeof(file), "%s\\WINBOOT.TTP", guest[i]); assert(CopyFileA(argv[7], file, FALSE));
        snprintf(file, sizeof(file), "%s\\MINT\\1-19-CUR\\MINT.CNF", guest[i]);
        f = fopen(file, "ab"); assert(f); fputs("\r\nexec u:/c/WINBOOT.TTP\r\n", f); fclose(f);
        snprintf(ini, sizeof(ini), "%s\\%s\\settings.ini", appdata, argv[i*2+1]);
        assert(WritePrivateProfileStringA("FolderD", "Path", folder, ini));
        assert(WritePrivateProfileStringA("HardDisk1", "Path", argv[8], ini));
        assert(WritePrivateProfileStringA("FloppyA", "Path", argv[9], ini));
        snprintf(file, sizeof(file), "%s\\shinogi.exe", dir[i]);
        snprintf(cmd, sizeof(cmd), "\"%s\" none", file); si.cb = sizeof(si);
        assert(CreateProcessA(file, cmd, NULL, NULL, FALSE, 0, NULL, NULL, &si, &running[i]));
        CloseHandle(running[i].hThread);
    }
    for (i = 0; i < 3; i++) {
        char file[1024], text[100] = {0};
        FILE *f = NULL;
        int n;
        snprintf(file, sizeof(file), "%s\\WINBOOT.LOG", guest[i]);
        for (n = 0; n < 1200; n++) {
            assert(WaitForSingleObject(running[i].hProcess, 0) == WAIT_TIMEOUT);
            f = fopen(file, "rb");
            if (f) { fgets(text, sizeof(text), f); fclose(f); if (strstr(text, "WINDOWS GUEST")) break; }
            Sleep(100);
        }
        assert(strstr(text, "WINDOWS GUEST PASS"));
        snprintf(file, sizeof(file), "%s\\EXTRA\\WINOUT.TXT", guest[i]);
        f = fopen(file, "rb"); assert(f); assert(fgets(text, sizeof(text), f)); fclose(f);
        assert(!strcmp(text, "WINDOWS WRITE\n"));
        printf("PASS actual Windows launcher/helper/QEMU guest boot and folder/image/floppy access: %s\n", argv[i*2+1]); fflush(stdout);
    }
    /* Upgrade one, then uninstall it: the other two and their links survive. */
    execute(argv[2], "/S");
    assert(WaitForSingleObject(running[0].hProcess, 0) == WAIT_OBJECT_0);
    for (i = 1; i < 3; i++) assert(WaitForSingleObject(running[i].hProcess, 0) == WAIT_TIMEOUT);
    CloseHandle(running[0].hProcess);
    {
        char uninstaller[512], settings[512], drive[MAX_PATH];
        snprintf(uninstaller, sizeof(uninstaller), "%s\\uninstall.exe", dir[0]);
        execute(uninstaller, "/S");
        /* NSIS uninstaller may re-exec from TEMP; wait for actual removal. */
        for (i = 0; i < 300 && GetFileAttributesA(uninstaller) != INVALID_FILE_ATTRIBUTES; i++) Sleep(100);
        assert(GetFileAttributesA(uninstaller) == INVALID_FILE_ATTRIBUTES);
        snprintf(settings, sizeof(settings), "%s\\%s\\settings.ini", appdata, argv[1]);
        assert(GetPrivateProfileStringA("Drives", "C", "", drive, sizeof(drive), settings));
        assert(GetFileAttributesA(drive) != INVALID_FILE_ATTRIBUTES);
    }
    for (i = 1; i < 3; i++) {
        char path[1024], actual[MAX_PATH]; installed(argv[i*2+1], actual);
        snprintf(path, sizeof(path), "%s\\shinogi.exe", actual);
        assert(GetFileAttributesA(path) != INVALID_FILE_ATTRIBUTES);
        shortcut(desktop, argv[i*2+1], path);
    }
    for (i = 1; i < 3; i++) {
        char file[1024]; snprintf(file, sizeof(file), "%s\\shinogi.exe", dir[i]);
        execute(file, "--stop-edition");
        assert(WaitForSingleObject(running[i].hProcess, 10000) == WAIT_OBJECT_0);
        CloseHandle(running[i].hProcess);
    }
    puts("PASS: three independent program/default-drive/settings folders, desktop/Start links, upgrade and isolated uninstall; guest files retained");
    CoUninitialize();
    return 0;
}

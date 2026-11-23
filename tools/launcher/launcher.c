/*
 * MotorMash.exe - single-file launcher for the Motor Mash (1998) modern-hardware port.
 *
 * Drop it into the original install folder (the one with MotorMsh.exe, Main\, Track\
 * and Set.ini) and run it. It carries everything else as embedded resources:
 *   - the patched game exe (crash fix, CD-music import redirect, window style)
 *   - MMCDA.dll (CD music, widescreen, HUD, camera zoom, interpolation, fixes)
 *   - dgVoodoo2 DDraw.dll / D3DImm.dll + dgVoodoo.conf
 *   - the DirectX 5 d3drm*.dll / d3dxof.dll from the game disc
 *   - the CD soundtrack as Ogg Vorbis
 * On launch these are unpacked (once per version) to
 *   %LOCALAPPDATA%\MotorMashPort\<build id>\
 * and the game is started from there with the install folder as its working
 * directory, so it reads its data exactly where the installer put it. Nothing in the
 * install folder is added or changed (apart from the game's own save/settings files).
 * Settings and logs (mmcda.ini, mmcda.log) live in %LOCALAPPDATA%\MotorMashPort\.
 */
#define WIN32_LEAN_AND_MEAN
#define _CRT_SECURE_NO_WARNINGS
#include <windows.h>
#include <shellapi.h>
#include <shlobj.h>
#include <stdio.h>
#include <wchar.h>
#include "payload.h"   /* generated: BUILD_ID, PAYLOAD_COUNT, g_payload[] */

static void fail(const wchar_t *msg)
{
    MessageBoxW(NULL, msg, L"Motor Mash", MB_ICONERROR | MB_OK);
    ExitProcess(1);
}

static int exists(const wchar_t *p) { return GetFileAttributesW(p) != INVALID_FILE_ATTRIBUTES; }

static void make_dirs(wchar_t *path)   /* create every directory in path (in place) */
{
    wchar_t *s;
    for (s = path + 3; *s; s++) {
        if (*s == L'\\') { *s = 0; CreateDirectoryW(path, NULL); *s = L'\\'; }
    }
    CreateDirectoryW(path, NULL);
}

static void unpack(const wchar_t *cache)
{
    int i;
    for (i = 0; i < PAYLOAD_COUNT; i++) {
        HRSRC r = FindResourceW(NULL, MAKEINTRESOURCEW(g_payload[i].id), (LPCWSTR)RT_RCDATA);
        HGLOBAL h;
        const void *data;
        DWORD size, wrote;
        wchar_t path[MAX_PATH], dir[MAX_PATH], *slash;
        WIN32_FILE_ATTRIBUTE_DATA fa;
        HANDLE f;
        if (!r || !(h = LoadResource(NULL, r)) || !(data = LockResource(h))) fail(L"The launcher is damaged (missing embedded file).");
        size = SizeofResource(NULL, r);
        swprintf(path, MAX_PATH, L"%s\\%s", cache, g_payload[i].name);
        if (GetFileAttributesExW(path, GetFileExInfoStandard, &fa) &&
            fa.nFileSizeHigh == 0 && fa.nFileSizeLow == size)
            continue;                                   /* already unpacked */
        wcscpy(dir, path);
        if ((slash = wcsrchr(dir, L'\\'))) { *slash = 0; make_dirs(dir); }
        f = CreateFileW(path, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
        if (f == INVALID_HANDLE_VALUE) fail(L"Could not write the game files to %LOCALAPPDATA%\\MotorMashPort.");
        if (!WriteFile(f, data, size, &wrote, NULL) || wrote != size) { CloseHandle(f); DeleteFileW(path); fail(L"Could not write the game files (disk full?)."); }
        CloseHandle(f);
    }
}

/* remove unpacked folders of older launcher versions (only our build-* folders) */
static void prune_old(const wchar_t *base)
{
    wchar_t pat[MAX_PATH];
    WIN32_FIND_DATAW fd;
    HANDLE h;
    swprintf(pat, MAX_PATH, L"%s\\build-*", base);
    h = FindFirstFileW(pat, &fd);
    if (h == INVALID_HANDLE_VALUE) return;
    do {
        if ((fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) && wcscmp(fd.cFileName, BUILD_ID) != 0) {
            wchar_t dir[MAX_PATH + 2];
            SHFILEOPSTRUCTW op = {0};
            swprintf(dir, MAX_PATH, L"%s\\%s", base, fd.cFileName);
            dir[wcslen(dir) + 1] = 0;                     /* double-NUL terminated */
            op.wFunc = FO_DELETE;
            op.pFrom = dir;
            op.fFlags = FOF_NO_UI;
            SHFileOperationW(&op);
        }
    } while (FindNextFileW(h, &fd));
    FindClose(h);
}

int WINAPI wWinMain(HINSTANCE inst, HINSTANCE prev, LPWSTR args, int show)
{
    wchar_t gamedir[MAX_PATH], probe[MAX_PATH], base[MAX_PATH], cache[MAX_PATH], exe[MAX_PATH];
    wchar_t cmd[2 * MAX_PATH], *slash, *local = NULL;
    STARTUPINFOW si = { sizeof si };
    PROCESS_INFORMATION pi;

    /* the install folder is wherever this launcher was placed */
    GetModuleFileNameW(NULL, gamedir, MAX_PATH);
    if ((slash = wcsrchr(gamedir, L'\\'))) *slash = 0;
    swprintf(probe, MAX_PATH, L"%s\\Main", gamedir);
    if (!exists(probe)) {
        swprintf(probe, MAX_PATH, L"%s\\Track", gamedir);
        if (!exists(probe))
            fail(L"Put MotorMash.exe in your Motor Mash install folder\n"
                 L"(the folder that contains MotorMsh.exe and the Main and Track folders), then run it from there.");
    }

    if (FAILED(SHGetKnownFolderPath(&FOLDERID_LocalAppData, 0, NULL, &local))) fail(L"Could not find %LOCALAPPDATA%.");
    swprintf(base, MAX_PATH, L"%s\\MotorMashPort", local);
    CoTaskMemFree(local);
    swprintf(cache, MAX_PATH, L"%s\\%s", base, BUILD_ID);
    make_dirs(cache);
    prune_old(base);
    unpack(cache);

    /* settings + log live in the version-independent folder */
    swprintf(probe, MAX_PATH, L"%s\\", base);
    SetEnvironmentVariableW(L"MMCDA_HOME", probe);

    swprintf(exe, MAX_PATH, L"%s\\MotorMsh.exe", cache);
    swprintf(cmd, 2 * MAX_PATH, L"\"%s\" %s", exe, args ? args : L"");
    if (!CreateProcessW(exe, cmd, NULL, NULL, FALSE, 0, NULL, gamedir, &si, &pi))
        fail(L"Could not start the game.");
    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);
    return 0;
}

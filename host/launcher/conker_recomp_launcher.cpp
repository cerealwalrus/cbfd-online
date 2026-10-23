// ConkerRecomp.exe (Windows): the game is ConkerOnline.exe now. This small program, under the old name, starts it from the
// same folder with the same arguments and ends, so what still starts the old name keeps working: shortcuts, and the
// updater of an older version, which starts the program it replaced (it would otherwise start the old game again, and
// that one would offer the update once more).
#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include <string>

int WINAPI WinMain(HINSTANCE, HINSTANCE, LPSTR, int) {
    wchar_t own[MAX_PATH * 2] = {};
    const DWORD n = GetModuleFileNameW(nullptr, own, (DWORD)(sizeof(own) / sizeof(own[0])));
    std::wstring folder(own, n);
    const size_t slash = folder.find_last_of(L"\\/");
    folder = slash == std::wstring::npos ? std::wstring(L".") : folder.substr(0, slash);
    const std::wstring game = folder + L"\\ConkerOnline.exe";
    // the arguments as they were given: what follows this program's own name in its command line
    std::wstring rest;
    {
        const wchar_t* line = GetCommandLineW();
        bool quoted = false;
        while (*line != 0 && (quoted || (*line != L' ' && *line != L'\t'))) {
            if (*line == L'"') {
                quoted = !quoted;
            }
            line++;
        }
        rest = line;
    }
    std::wstring command = L"\"" + game + L"\"" + rest;
    STARTUPINFOW si{};
    si.cb = sizeof(si);
    PROCESS_INFORMATION pi{};
    if (!CreateProcessW(game.c_str(), &command[0], nullptr, nullptr, FALSE, 0, nullptr, folder.c_str(), &si, &pi)) {
        MessageBoxW(nullptr, L"ConkerOnline.exe, the game, wasn't found next to ConkerRecomp.exe. Unpack the whole release into this folder again.",
            L"Conker's Bad Fur Day: Recompiled", MB_OK | MB_ICONERROR);
        return 1;
    }
    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);
    return 0;
}

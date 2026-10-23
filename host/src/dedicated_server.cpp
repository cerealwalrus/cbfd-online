// A dedicated server run from the launcher or the command line. The server is the game itself with
// --dedicated (headless, no player slot, see main.cpp and net_session.cpp). A session is one run of the
// game, so a supervisor process starts it again whenever it ends (everybody left, or it crashed): the
// server stays up until whoever runs it closes the application. Both processes sit in a job object,
// so closing the launcher (or the server's console) ends them with it.
#include <chrono>
#include <cstdarg>
#include <cstdio>
#include <ctime>
#include <string>
#include <thread>

#include "conker.hpp"

#if defined(_WIN32)
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <Windows.h>

namespace {
    HANDLE launched_job = nullptr;    // the launcher's server (supervisor and game inside)

    HANDLE make_job() {
        HANDLE job = CreateJobObjectW(nullptr, nullptr);
        if (job) {
            JOBOBJECT_EXTENDED_LIMIT_INFORMATION info{};
            info.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
            SetInformationJobObject(job, JobObjectExtendedLimitInformation, &info, sizeof(info));
        }
        return job;
    }

    std::wstring widen(const std::string& text) {
        if (text.empty()) {
            return {};
        }
        int n = MultiByteToWideChar(CP_UTF8, 0, text.data(), (int)text.size(), nullptr, 0);
        std::wstring out(n, L'\0');
        MultiByteToWideChar(CP_UTF8, 0, text.data(), (int)text.size(), out.data(), n);
        return out;
    }
}

bool conker::dserver::running() {
    return launched_job != nullptr;
}

bool conker::dserver::start(const std::string& rom_path, int port, int players, int delay, std::string* error) {
    if (launched_job) {
        return true;
    }
    wchar_t exe[MAX_PATH * 2];
    DWORD n = GetModuleFileNameW(nullptr, exe, (DWORD)(sizeof(exe) / sizeof(exe[0])));
    if (n == 0) {
        if (error) *error = "Can't find this program's own file.";
        return false;
    }
    std::wstring cmd = L"\"" + std::wstring(exe, n) + L"\" --dedicated --rom \"" + widen(rom_path) + L"\" --port " +
        std::to_wstring(port) + L" --players " + std::to_wstring(players) + L" --delay " + std::to_wstring(delay);
    std::wstring folder(exe, n);
    folder = folder.substr(0, folder.find_last_of(L"\\/"));

    HANDLE job = make_job();
    if (!job) {
        if (error) *error = "Windows wouldn't make a job for the server.";
        return false;
    }
    STARTUPINFOW si{};
    si.cb = sizeof(si);
    PROCESS_INFORMATION pi{};
    if (!CreateProcessW(nullptr, cmd.data(), nullptr, nullptr, FALSE, CREATE_NO_WINDOW | CREATE_SUSPENDED, nullptr, folder.c_str(), &si, &pi)) {
        if (error) *error = "Couldn't start the server program (error " + std::to_string(GetLastError()) + ").";
        CloseHandle(job);
        return false;
    }
    AssignProcessToJobObject(job, pi.hProcess);
    ResumeThread(pi.hThread);
    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);
    launched_job = job;
    std::printf("[server] dedicated server started from the launcher (port %d, %d players)\n", port, players);
    return true;
}

void conker::dserver::stop() {
    if (launched_job) {
        CloseHandle(launched_job);   // kill-on-close ends the supervisor and the game
        launched_job = nullptr;
        std::printf("[server] dedicated server stopped\n");
    }
}

namespace {
    // Every start and end of the server's game, with the time and the exit code, in netplay_supervisor.txt (beside the logs):
    // a restart the players didn't ask for shows here.
    void supervisor_note(const char* format, ...) {
        char text[400];
        va_list args;
        va_start(args, format);
        std::vsnprintf(text, sizeof(text), format, args);
        va_end(args);
        std::time_t now = std::time(nullptr);
        char when[32] = "";
        std::strftime(when, sizeof(when), "%Y-%m-%d %H:%M:%S", std::localtime(&now));
        std::printf("[server] %s\n", text);
        if (FILE* file = std::fopen("netplay_supervisor.txt", "a")) {
            std::fprintf(file, "%s %s\n", when, text);
            std::fclose(file);
        }
    }
}

int conker::dserver::supervise() {
    HANDLE job = make_job();
    SetEnvironmentVariableW(L"CONKER_DEDICATED_CHILD", L"1");
    std::wstring cmd = GetCommandLineW();
    int quick_failures = 0;
    supervisor_note("supervisor started: the server restarts whenever its game ends");
    for (;;) {
        STARTUPINFOW si{};
        si.cb = sizeof(si);
        PROCESS_INFORMATION pi{};
        std::wstring line = cmd;
        const auto began = std::chrono::steady_clock::now();
        if (!CreateProcessW(nullptr, line.data(), nullptr, nullptr, TRUE, CREATE_SUSPENDED, nullptr, nullptr, &si, &pi)) {
            supervisor_note("couldn't start the server's game (error %lu)", GetLastError());
            return 1;
        }
        if (job) {
            AssignProcessToJobObject(job, pi.hProcess);
        }
        ResumeThread(pi.hThread);
        CloseHandle(pi.hThread);
        supervisor_note("the server's game started (process %lu)", (unsigned long)pi.dwProcessId);
        WaitForSingleObject(pi.hProcess, INFINITE);
        DWORD code = 0;
        GetExitCodeProcess(pi.hProcess, &code);
        CloseHandle(pi.hProcess);
        const double ran = std::chrono::duration<double>(std::chrono::steady_clock::now() - began).count();
        supervisor_note("the server's game ended after %.0f s, exit code 0x%lX: starting a new one", ran, (unsigned long)code);
        quick_failures = ran < 15.0 ? quick_failures + 1 : 0;
        if (quick_failures >= 5) {
            supervisor_note("the server keeps stopping right after it starts (wrong or missing ROM?): giving up");
            return 1;
        }
        std::this_thread::sleep_for(std::chrono::seconds(quick_failures ? 5 : 1));
    }
}

#else

bool conker::dserver::running() { return false; }
bool conker::dserver::start(const std::string&, int, int, int, std::string* error) {
    if (error) *error = "Starting a server from the launcher is Windows-only.";
    return false;
}
void conker::dserver::stop() {}
int conker::dserver::supervise() { return -1; }   // not supervised: run it under a loop or service

#endif

// Looks, once a run and in the background, for a newer release of the online build on GitHub (the latest release's tag), so the launcher
// can say there is an update. On Windows it can also install it (start_install): download the release's .zip, check its SHA-256 when GitHub
// gives one, then a small batch script waits for the game to close, unpacks the zip over the game's folder (never touching the ROM) and
// starts the game again.

#include <algorithm>
#include <atomic>
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <winhttp.h>
#include <bcrypt.h>
#endif

#include <SDL.h>

#include "conker.hpp"
#include "version.hpp"

namespace {
    std::atomic<bool> started{ false };
    std::atomic<bool> finished{ false };
    std::mutex result_mutex;
    std::string latest_tag;   // the newest release's tag, as published ("v0.6.1")
    std::string asset_url;    // the release's first .zip (browser_download_url)
    std::string asset_digest; // "sha256:..." as GitHub gives it, or empty
    std::mutex status_mutex;
    conker::updates::Status install_status;
    std::atomic<bool> installing{ false };

    std::string repo_name() {
        const char* text = SDL_getenv("CONKER_UPDATE_REPO");
        return text != nullptr && *text != 0 ? std::string(text) : std::string(CONKER_UPDATE_REPO);
    }

    // The first number-and-dot run of a tag, as numbers ("v0.6.1" is 0, 6, 1).
    std::vector<int> parse_version(const std::string& text) {
        std::vector<int> parts;
        size_t i = 0;
        while (i < text.size() && (text[i] < '0' || text[i] > '9')) {
            i++;
        }
        int current = 0;
        bool have = false;
        for (; i < text.size(); i++) {
            const char c = text[i];
            if (c >= '0' && c <= '9') {
                current = current * 10 + (c - '0');
                have = true;
            }
            else if (c == '.' && have) {
                parts.push_back(current);
                current = 0;
                have = false;
            }
            else {
                break;
            }
        }
        if (have) {
            parts.push_back(current);
        }
        return parts;
    }

    bool is_newer(const std::string& tag, const std::string& current) {
        const std::vector<int> a = parse_version(tag), b = parse_version(current);
        if (a.empty() || b.empty()) {
            return false;
        }
        for (size_t i = 0; i < std::max(a.size(), b.size()); i++) {
            const int x = i < a.size() ? a[i] : 0, y = i < b.size() ? b[i] : 0;
            if (x != y) {
                return x > y;
            }
        }
        return false;
    }

    // "tag_name": "v0.6.1" in the release's JSON
    std::string find_tag(const std::string& json) {
        const size_t key = json.find("\"tag_name\"");
        if (key == std::string::npos) {
            return {};
        }
        const size_t open = json.find('"', json.find(':', key) + 1);
        if (open == std::string::npos) {
            return {};
        }
        const size_t close = json.find('"', open + 1);
        if (close == std::string::npos || close - open > 64) {
            return {};
        }
        return json.substr(open + 1, close - open - 1);
    }

    // The first release file whose name ends in .zip: its download address and its "digest" (sha256:...), if GitHub gives one.
    void find_asset(const std::string& json, std::string* url, std::string* digest) {
        size_t at = 0, previous = 0;
        while ((at = json.find("\"browser_download_url\"", at)) != std::string::npos) {
            const size_t open = json.find('"', json.find(':', at) + 1);
            const size_t close = open == std::string::npos ? open : json.find('"', open + 1);
            if (close == std::string::npos) {
                return;
            }
            const std::string found = json.substr(open + 1, close - open - 1);
            at = close;
            const size_t before = previous;
            previous = close;
            if (found.size() > 4 && found.compare(found.size() - 4, 4, ".zip") == 0 && found.rfind("https://", 0) == 0) {
                *url = found;
                // GitHub lists "digest" in the asset's fields just before its download address (and after the previous asset's)
                const size_t d = json.rfind("\"digest\"", close);
                if (d != std::string::npos && d > before) {
                    const size_t o = json.find('"', json.find(':', d) + 1);
                    const size_t c = o == std::string::npos ? o : json.find('"', o + 1);
                    if (c != std::string::npos && c - o < 100) {
                        *digest = json.substr(o + 1, c - o - 1);
                    }
                }
                return;
            }
        }
    }

    std::string fetch_latest_release_json() {
        std::string body;
        const std::string path = "/repos/" + repo_name() + "/releases/latest";
#ifdef _WIN32
        const std::wstring wide_path(path.begin(), path.end());
        HINTERNET session = WinHttpOpen(L"ConkerRecomp-update-check", WINHTTP_ACCESS_TYPE_DEFAULT_PROXY, WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
        if (session == nullptr) {
            return body;
        }
        WinHttpSetTimeouts(session, 5000, 5000, 5000, 8000);
        HINTERNET connection = WinHttpConnect(session, L"api.github.com", INTERNET_DEFAULT_HTTPS_PORT, 0);
        HINTERNET request = connection != nullptr ? WinHttpOpenRequest(connection, L"GET", wide_path.c_str(), nullptr, WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES, WINHTTP_FLAG_SECURE) : nullptr;
        if (request != nullptr) {
            const wchar_t* headers = L"Accept: application/vnd.github+json";
            if (WinHttpSendRequest(request, headers, (DWORD)-1L, WINHTTP_NO_REQUEST_DATA, 0, 0, 0) && WinHttpReceiveResponse(request, nullptr)) {
                DWORD status = 0, size = sizeof(status);
                WinHttpQueryHeaders(request, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER, WINHTTP_HEADER_NAME_BY_INDEX, &status, &size, WINHTTP_NO_HEADER_INDEX);
                if (status == 200) {
                    for (;;) {
                        DWORD available = 0;
                        if (!WinHttpQueryDataAvailable(request, &available) || available == 0 || body.size() > 262144) {
                            break;
                        }
                        std::string chunk(available, '\0');
                        DWORD read = 0;
                        if (!WinHttpReadData(request, &chunk[0], available, &read) || read == 0) {
                            break;
                        }
                        body.append(chunk, 0, read);
                    }
                }
            }
            WinHttpCloseHandle(request);
        }
        if (connection != nullptr) {
            WinHttpCloseHandle(connection);
        }
        WinHttpCloseHandle(session);
#else
        const std::string command = "curl -sf -m 8 -H 'Accept: application/vnd.github+json' -H 'User-Agent: ConkerRecomp-update-check' 'https://api.github.com" + path + "' 2>/dev/null";
        FILE* pipe = popen(command.c_str(), "r");
        if (pipe != nullptr) {
            char buffer[4096];
            size_t n;
            while ((n = std::fread(buffer, 1, sizeof(buffer), pipe)) > 0 && body.size() < 262144) {
                body.append(buffer, n);
            }
            pclose(pipe);
        }
#endif
        return body;
    }
}

void conker::updates::check_in_background() {
    if (started.exchange(true)) {
        return;
    }
    if (SDL_getenv("CONKER_NO_UPDATE_CHECK") != nullptr) {
        finished = true;
        return;
    }
    std::thread([]() {
        const std::string json = fetch_latest_release_json();
        const std::string tag = find_tag(json);
        std::string url, digest;
        find_asset(json, &url, &digest);
        {
            std::lock_guard lock{ result_mutex };
            latest_tag = tag;
            asset_url = url;
            asset_digest = digest;
        }
        finished = true;
    }).detach();
}

bool conker::updates::newer_available(std::string* tag, std::string* page) {
    if (!finished.load()) {
        return false;
    }
    std::string found;
    {
        std::lock_guard lock{ result_mutex };
        found = latest_tag;
    }
    if (found.empty() || !is_newer(found, CONKER_ONLINE_VERSION)) {
        return false;
    }
    if (tag != nullptr) {
        *tag = found;
    }
    if (page != nullptr) {
        *page = "https://github.com/" + repo_name() + "/releases/latest";
    }
    return true;
}

const char* conker::updates::current_version() {
    return CONKER_ONLINE_VERSION;
}

// --- Installing ---

namespace {
    void set_status(int state, int percent, const std::string& message) {
        std::lock_guard lock{ status_mutex };
        install_status.state = state;
        install_status.percent = percent;
        install_status.message = message;
    }

#ifdef _WIN32
    std::wstring widen(const std::string& text) {
        if (text.empty()) {
            return {};
        }
        const int n = MultiByteToWideChar(CP_UTF8, 0, text.c_str(), (int)text.size(), nullptr, 0);
        std::wstring out(n, L'\0');
        MultiByteToWideChar(CP_UTF8, 0, text.c_str(), (int)text.size(), &out[0], n);
        return out;
    }
    std::string narrow(const std::wstring& text) {
        if (text.empty()) {
            return {};
        }
        const int n = WideCharToMultiByte(CP_UTF8, 0, text.c_str(), (int)text.size(), nullptr, 0, nullptr, nullptr);
        std::string out(n, '\0');
        WideCharToMultiByte(CP_UTF8, 0, text.c_str(), (int)text.size(), &out[0], n, nullptr, nullptr);
        return out;
    }

    // Downloads url to path (HTTPS, redirects followed). Returns an error text, or empty when done.
    std::string download(const std::string& url, const std::wstring& path) {
        const std::wstring wide_url = widen(url);
        URL_COMPONENTS parts{};
        parts.dwStructSize = sizeof(parts);
        wchar_t host[256] = {}, object[2048] = {};
        parts.lpszHostName = host;
        parts.dwHostNameLength = 256;
        parts.lpszUrlPath = object;
        parts.dwUrlPathLength = 2048;
        if (!WinHttpCrackUrl(wide_url.c_str(), 0, 0, &parts)) {
            return "the download address was not understood";
        }
        HINTERNET session = WinHttpOpen(L"ConkerRecomp-update", WINHTTP_ACCESS_TYPE_DEFAULT_PROXY, WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
        if (session == nullptr) {
            return "could not start the download";
        }
        WinHttpSetTimeouts(session, 10000, 10000, 15000, 30000);
        std::string error = "could not reach GitHub";
        HINTERNET connection = WinHttpConnect(session, host, parts.nPort, 0);
        HINTERNET request = connection != nullptr ? WinHttpOpenRequest(connection, L"GET", object, nullptr, WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES, WINHTTP_FLAG_SECURE) : nullptr;
        if (request != nullptr && WinHttpSendRequest(request, WINHTTP_NO_ADDITIONAL_HEADERS, 0, WINHTTP_NO_REQUEST_DATA, 0, 0, 0) && WinHttpReceiveResponse(request, nullptr)) {
            DWORD status = 0, size = sizeof(status);
            WinHttpQueryHeaders(request, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER, WINHTTP_HEADER_NAME_BY_INDEX, &status, &size, WINHTTP_NO_HEADER_INDEX);
            if (status != 200) {
                error = "GitHub answered " + std::to_string((int)status);
            }
            else {
                wchar_t length_text[32] = {};
                DWORD length_size = sizeof(length_text);
                uint64_t total = 0;
                if (WinHttpQueryHeaders(request, WINHTTP_QUERY_CONTENT_LENGTH, WINHTTP_HEADER_NAME_BY_INDEX, length_text, &length_size, WINHTTP_NO_HEADER_INDEX)) {
                    total = _wcstoui64(length_text, nullptr, 10);
                }
                std::ofstream file(std::filesystem::path(path), std::ios::binary | std::ios::trunc);
                if (!file) {
                    error = "could not write the downloaded file";
                }
                else {
                    uint64_t got = 0;
                    std::vector<char> buffer(65536);
                    error.clear();
                    for (;;) {
                        DWORD read = 0;
                        if (!WinHttpReadData(request, buffer.data(), (DWORD)buffer.size(), &read)) {
                            error = "the download was interrupted";
                            break;
                        }
                        if (read == 0) {
                            break;
                        }
                        file.write(buffer.data(), read);
                        got += read;
                        if (total > 0) {
                            set_status(1, (int)std::min<uint64_t>(99, got * 100 / total), "");
                        }
                    }
                    file.close();
                    if (error.empty() && (got == 0 || (total > 0 && got != total))) {
                        error = "the download was incomplete";
                    }
                }
            }
        }
        if (request != nullptr) {
            WinHttpCloseHandle(request);
        }
        if (connection != nullptr) {
            WinHttpCloseHandle(connection);
        }
        WinHttpCloseHandle(session);
        return error;
    }

    std::string sha256_of_file(const std::wstring& path) {
        std::string hex;
        BCRYPT_ALG_HANDLE alg = nullptr;
        BCRYPT_HASH_HANDLE hash = nullptr;
        if (BCryptOpenAlgorithmProvider(&alg, BCRYPT_SHA256_ALGORITHM, nullptr, 0) != 0) {
            return hex;
        }
        if (BCryptCreateHash(alg, &hash, nullptr, 0, nullptr, 0, 0) == 0) {
            std::ifstream file(std::filesystem::path(path), std::ios::binary);
            std::vector<char> buffer(65536);
            while (file) {
                file.read(buffer.data(), buffer.size());
                const std::streamsize n = file.gcount();
                if (n > 0) {
                    BCryptHashData(hash, reinterpret_cast<PUCHAR>(buffer.data()), (ULONG)n, 0);
                }
            }
            UCHAR digest[32] = {};
            if (BCryptFinishHash(hash, digest, 32, 0) == 0) {
                static const char* digits = "0123456789abcdef";
                for (UCHAR b : digest) {
                    hex += digits[b >> 4];
                    hex += digits[b & 15];
                }
            }
            BCryptDestroyHash(hash);
        }
        BCryptCloseAlgorithmProvider(alg, 0);
        return hex;
    }

    std::string batch_safe(const std::string& text) {
        std::string out;
        for (char c : text) {
            out += c;
            if (c == '%') {
                out += '%';
            }
        }
        return out;
    }

    // Writes the script that finishes the job after the game has closed, and starts it. Returns an error text, or empty.
    std::string start_helper(const std::wstring& work, const std::wstring& zip) {
        wchar_t exe_path[MAX_PATH * 2] = {};
        GetModuleFileNameW(nullptr, exe_path, MAX_PATH * 2);
        std::wstring exe = exe_path;
        const size_t slash = exe.find_last_of(L"\\/");
        if (slash == std::wstring::npos) {
            return "could not find the game's folder";
        }
        const std::wstring folder = exe.substr(0, slash);
        const std::wstring script = work + L"\\update.bat";
        const std::string w = batch_safe(narrow(work)), z = batch_safe(narrow(zip)), f = batch_safe(narrow(folder)), e = batch_safe(narrow(exe));
        std::ofstream bat(std::filesystem::path(script), std::ios::binary | std::ios::trunc);
        if (!bat) {
            return "could not write the update script";
        }
        bat << "@echo off\r\nchcp 65001 >nul\r\n"
            << "set \"WORK=" << w << "\"\r\n"
            << "set \"LOG=%WORK%\\update.log\"\r\n"
            << "echo update started > \"%LOG%\"\r\n"
            << "set TRIES=0\r\n"
            << ":wait\r\n"
            << "tasklist /FI \"PID eq " << GetCurrentProcessId() << "\" /NH /FO CSV 2>nul | find \",\"\"" << GetCurrentProcessId() << "\"\",\" >nul\r\n"
            << "if errorlevel 1 goto gone\r\n"
            << "set /a TRIES+=1\r\n"
            << "if %TRIES% GEQ 120 echo the game did not close in time >> \"%LOG%\"\r\n"
            << "if %TRIES% GEQ 120 goto fail\r\n"
            << "ping -n 2 127.0.0.1 >nul\r\n"
            << "goto wait\r\n"
            << ":gone\r\n"
            << "echo the game closed after %TRIES% waits >> \"%LOG%\"\r\n"
            << "rmdir /s /q \"%WORK%\\stage\" >nul 2>&1\r\n"
            << "mkdir \"%WORK%\\stage\"\r\n"
            << "tar -xf \"" << z << "\" -C \"%WORK%\\stage\" >> \"%LOG%\" 2>&1\r\n"
            << "if errorlevel 1 goto fail\r\n"
            << "echo unpacked >> \"%LOG%\"\r\n"
            << "set \"SRC=%WORK%\\stage\"\r\n"
            << "dir /a-d /b \"%WORK%\\stage\" >nul 2>&1 && goto copy\r\n"
            << "set N=0\r\n"
            << "for /d %%D in (\"%WORK%\\stage\\*\") do (set /a N+=1 & set \"ONE=%%~fD\")\r\n"
            << "if \"%N%\"==\"1\" set \"SRC=%ONE%\"\r\n"
            << ":copy\r\n"
            << "robocopy \"%SRC%\" \"" << f << "\" /E /R:10 /W:1 /XF *.z64 *.n64 *.v64 *.rom update.bat >> \"%LOG%\" 2>&1\r\n"
            << "echo robocopy answered %ERRORLEVEL% >> \"%LOG%\"\r\n"
            << "if %ERRORLEVEL% GEQ 8 goto fail\r\n"
            << "echo copied from %SRC% >> \"%LOG%\"\r\n"
            << "echo done >> \"%LOG%\"\r\n"
            << "cd /d \"" << f << "\"\r\n"
            // (the game is ConkerOnline.exe now: started if the update brought it, else the program that ran)
            << "if exist \"" << f << "\\ConkerOnline.exe\" (start \"\" \"" << f << "\\ConkerOnline.exe\") else (start \"\" \"" << e << "\")\r\n"
            << "rmdir /s /q \"%WORK%\\stage\" >nul 2>&1\r\n"
            << "del \"" << z << "\" >nul 2>&1\r\n"
            << "exit /b 0\r\n"
            << ":fail\r\n"
            << "echo the update did not finish >> \"%LOG%\"\r\n"
            << "cd /d \"" << f << "\"\r\n"
            << "if exist \"" << f << "\\ConkerOnline.exe\" (start \"\" \"" << f << "\\ConkerOnline.exe\") else (start \"\" \"" << e << "\")\r\n"
            << "start \"\" notepad \"%LOG%\"\r\n"
            << "exit /b 1\r\n";
        bat.close();
        std::wstring command = L"cmd.exe /c \"\"" + script + L"\"\"";
        STARTUPINFOW startup{};
        startup.cb = sizeof(startup);
        PROCESS_INFORMATION info{};
        if (!CreateProcessW(nullptr, &command[0], nullptr, nullptr, FALSE, CREATE_NO_WINDOW | CREATE_NEW_PROCESS_GROUP, nullptr, work.c_str(), &startup, &info)) {
            return "could not start the update script";
        }
        CloseHandle(info.hThread);
        CloseHandle(info.hProcess);
        return {};
    }
#endif
}

bool conker::updates::install_possible() {
#ifdef _WIN32
    std::lock_guard lock{ result_mutex };
    return !asset_url.empty();
#else
    return false;
#endif
}

void conker::updates::start_install() {
#ifdef _WIN32
    if (installing.exchange(true)) {
        return;
    }
    std::string url, digest;
    {
        std::lock_guard lock{ result_mutex };
        url = asset_url;
        digest = asset_digest;
    }
    set_status(1, 0, "");
    std::thread([url, digest]() {
        wchar_t temp[MAX_PATH] = {};
        GetTempPathW(MAX_PATH, temp);
        const std::wstring work = std::wstring(temp) + L"conker_update";
        CreateDirectoryW(work.c_str(), nullptr);
        const std::wstring zip = work + L"\\update.zip";
        std::string error = url.empty() ? "the release has no .zip file" : download(url, zip);
        if (error.empty()) {
            set_status(2, 100, "");
            if (digest.rfind("sha256:", 0) == 0) {
                std::string want = digest.substr(7);
                std::string have = sha256_of_file(zip);
                std::transform(want.begin(), want.end(), want.begin(), [](unsigned char c) { return (char)std::tolower(c); });
                if (have != want) {
                    error = "the download did not match GitHub's checksum";
                }
            }
        }
        if (error.empty()) {
            error = start_helper(work, zip);
        }
        if (!error.empty()) {
            set_status(3, 0, error);
            installing = false;
            return;
        }
        set_status(4, 100, "");   // ready: the game closes, the script takes over
    }).detach();
#endif
}

conker::updates::Status conker::updates::status() {
    std::lock_guard lock{ status_mutex };
    return install_status;
}

// Looks, once a run and in the background, for a newer release of the online build on GitHub (the latest release's tag), so the launcher
// can say there is an update. Nothing is downloaded or changed here.

#include <algorithm>
#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <winhttp.h>
#endif

#include <SDL.h>

#include "conker.hpp"
#include "version.hpp"

namespace {
    std::atomic<bool> started{ false };
    std::atomic<bool> finished{ false };
    std::mutex result_mutex;
    std::string latest_tag;   // the newest release's tag, as published ("v0.6.1")

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
        const std::string tag = find_tag(fetch_latest_release_json());
        {
            std::lock_guard lock{ result_mutex };
            latest_tag = tag;
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

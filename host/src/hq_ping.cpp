// (0.10.45) The launcher status bar's PING, every 5 seconds while the launcher is up, on a thread of its own. Windows only (elsewhere it
// stays "--").
// (0.10.47) To GitHub again, api.github.com (the update check's), which doesn't answer ICMP echoes: the time a TCP connection to its
// port 443 takes to open (one round trip, as a ping's). CONKER_PING_HOST changes the host.

#if defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#endif

#include <atomic>
#include <chrono>
#include <cstdio>
#include <string>
#include <thread>

#include <SDL.h>

#include "conker.hpp"

namespace {
    std::atomic<int> result{ -2 };
    std::atomic<long long> wanted_until{ 0 };
    std::atomic<bool> started{ false };

    long long now_ms() {
        return std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now().time_since_epoch()).count();
    }

    std::string host_name() {
        const char* text = SDL_getenv("CONKER_PING_HOST");
        return text != nullptr && *text != 0 ? std::string(text) : std::string("api.github.com");
    }

#if defined(_WIN32)
    // one TCP connection opened (and closed at once): the time it took in ms, or -1 (no answer within 2 s, or the name didn't resolve)
    int ping_once(const std::string& host) {
        addrinfo hints{};
        hints.ai_family = AF_INET;
        hints.ai_socktype = SOCK_STREAM;
        addrinfo* found = nullptr;
        if (getaddrinfo(host.c_str(), "443", &hints, &found) != 0 || found == nullptr) {
            return -1;
        }
        sockaddr_in address = *(sockaddr_in*)found->ai_addr;
        freeaddrinfo(found);
        SOCKET s = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
        if (s == INVALID_SOCKET) {
            return -1;
        }
        u_long non_blocking = 1;
        ioctlsocket(s, FIONBIO, &non_blocking);
        int ms = -1;
        const auto sent_at = std::chrono::steady_clock::now();
        if (connect(s, (sockaddr*)&address, sizeof(address)) == 0 || WSAGetLastError() == WSAEWOULDBLOCK) {
            fd_set write_set, error_set;
            FD_ZERO(&write_set);
            FD_ZERO(&error_set);
            FD_SET(s, &write_set);
            FD_SET(s, &error_set);
            timeval wait{ 2, 0 };
            if (select(0, nullptr, &write_set, &error_set, &wait) > 0 && FD_ISSET(s, &write_set) && !FD_ISSET(s, &error_set)) {
                ms = (int)std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - sent_at).count();
            }
        }
        closesocket(s);
        return ms;
    }
#endif
}

void conker::hq_ping::keep_going() {
    wanted_until = now_ms() + 3000;
#if defined(_WIN32)
    if (started.exchange(true)) {
        return;
    }
    std::thread([]() {
        WSADATA wsa;
        WSAStartup(MAKEWORD(2, 2), &wsa);
        const std::string host = host_name();
        bool logged = false;
        for (;;) {
            if (now_ms() < wanted_until.load()) {
                const int ms = ping_once(host);
                result = ms;
                if (!logged) {
                    logged = true;
                    std::printf("[ping] %s: %s\n", host.c_str(), ms >= 0 ? (std::to_string(ms) + " ms").c_str() : "no answer");
                }
                std::this_thread::sleep_for(std::chrono::seconds(5));
            }
            else {
                std::this_thread::sleep_for(std::chrono::milliseconds(500));
            }
        }
    }).detach();
#endif
}

int conker::hq_ping::last_ms() {
    return result.load();
}

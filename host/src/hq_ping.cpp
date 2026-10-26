// (0.10.45) The launcher status bar's PING: an ICMP echo (a real ping) to the tavern, conkerstavern.servebeer.com (CONKER_PING_HOST
// changes it), every 5 seconds while the launcher is up, on a thread of its own. Windows only (elsewhere it stays "--").

#if defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#include <iphlpapi.h>
#include <icmpapi.h>
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
        return text != nullptr && *text != 0 ? std::string(text) : std::string("conkerstavern.servebeer.com");
    }

#if defined(_WIN32)
    // one echo: the round trip in ms, or -1 (no answer, or the name didn't resolve)
    int ping_once(const std::string& host) {
        addrinfo hints{};
        hints.ai_family = AF_INET;
        hints.ai_socktype = SOCK_DGRAM;
        addrinfo* found = nullptr;
        if (getaddrinfo(host.c_str(), nullptr, &hints, &found) != 0 || found == nullptr) {
            return -1;
        }
        const IPAddr address = ((sockaddr_in*)found->ai_addr)->sin_addr.S_un.S_addr;
        freeaddrinfo(found);
        HANDLE icmp = IcmpCreateFile();
        if (icmp == INVALID_HANDLE_VALUE) {
            return -1;
        }
        char data[32] = "conker field launcher ping";
        alignas(8) char reply[sizeof(ICMP_ECHO_REPLY) + sizeof(data) + 8 + 64] = {};
        int ms = -1;
        const ULONGLONG sent_at = GetTickCount64();
        if (IcmpSendEcho(icmp, address, data, (WORD)sizeof(data), nullptr, reply, (DWORD)sizeof(reply), 1500) > 0) {
            const ICMP_ECHO_REPLY* echo = (const ICMP_ECHO_REPLY*)reply;
            if (echo->Status == IP_SUCCESS) {
                // (RoundTripTime is 0 for under a millisecond; the tick count is the fallback if it looks wrong)
                ms = (int)echo->RoundTripTime;
                const int ticks = (int)(GetTickCount64() - sent_at);
                if (ms > ticks + 20) {
                    ms = ticks;
                }
            }
        }
        IcmpCloseHandle(icmp);
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

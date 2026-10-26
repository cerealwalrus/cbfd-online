// (0.10.46) The launcher status bar's NETCODE: whether others can reach this machine's UDP port 47000 (the port a host listens on),
// that is whether this player can host. On Windows, as the launcher starts, a thread:
//   1. finds the router over UPnP (SSDP), reads its description and its WANIPConnection (or WANPPPConnection) service;
//   2. asks it for its internet address: a private or carrier-grade one (10.x, 172.16-31.x, 192.168.x, 100.64-127.x) means the router
//      is behind another NAT, and nobody outside can reach the port, whatever's forwarded;
//   3. maps UDP 47000 to this machine (AddPortMapping, a 2 hour lease renewed every 30 minutes while the program runs; a permanent
//      one where the router only takes those), unless CONKER_NO_UPNP=1;
//   4. reads the mapping back (GetSpecificPortMappingEntry): to this machine is OPEN, to another machine is taken.
// With no UPnP router it can't tell (a port forwarded by hand can only be confirmed from outside): UNVERIFIED.

#if defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#endif

#include <algorithm>
#include <atomic>
#include <cctype>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include <SDL.h>

#include "conker.hpp"

namespace {
    constexpr int game_port = 47000;
    std::mutex status_mutex;
    conker::upnp::Status current;
    std::atomic<bool> started{ false };

    void set_status(int state, const std::string& detail) {
        {
            std::lock_guard lock{ status_mutex };
            current.state = state;
            current.detail = detail;
        }
        static const char* const names[] = { "checking", "open", "closed", "unverified" };
        std::printf("[upnp] port %d: %s%s%s\n", game_port, names[state], detail.empty() ? "" : ", ", detail.c_str());
    }

#if defined(_WIN32)
    std::string lower(std::string text) {
        std::transform(text.begin(), text.end(), text.begin(), [](unsigned char c) { return (char)std::tolower(c); });
        return text;
    }

    std::string trim(const std::string& text) {
        size_t a = 0, b = text.size();
        while (a < b && std::isspace((unsigned char)text[a])) a++;
        while (b > a && std::isspace((unsigned char)text[b - 1])) b--;
        return text.substr(a, b - a);
    }

    // a header's value in an HTTP or SSDP message (names in any case)
    std::string header(const std::string& message, const std::string& name) {
        const std::string low = lower(message);
        const std::string key = "\r\n" + lower(name) + ":";
        size_t at = low.find(key);
        if (at == std::string::npos) {
            return {};
        }
        at += key.size();
        const size_t end = message.find("\r\n", at);
        return trim(message.substr(at, end == std::string::npos ? std::string::npos : end - at));
    }

    // the text of <tag>...</tag> (also <ns:tag>), from `from` on
    std::string xml_value(const std::string& xml, const std::string& tag, size_t from = 0, size_t* found_at = nullptr) {
        size_t at = from;
        for (;;) {
            at = xml.find(tag + ">", at);
            if (at == std::string::npos) {
                return {};
            }
            const char before = at > 0 ? xml[at - 1] : 0;
            if (before == '<' || before == ':') {
                if (before == ':' && (at < 2 || xml.rfind('<', at) == std::string::npos || xml[xml.rfind('<', at) + 1] == '/')) {
                    at += tag.size();
                    continue;
                }
                break;
            }
            at += tag.size();
        }
        const size_t start = at + tag.size() + 1;
        const size_t end = xml.find('<', start);
        if (found_at != nullptr) {
            *found_at = at;
        }
        return trim(xml.substr(start, end == std::string::npos ? std::string::npos : end - start));
    }

    struct Url {
        std::string host;
        int port = 80;
        std::string path = "/";
    };

    bool parse_url(const std::string& text, Url* url) {
        if (lower(text.substr(0, 7)) != "http://") {
            return false;
        }
        const size_t slash = text.find('/', 7);
        const std::string host_port = text.substr(7, slash == std::string::npos ? std::string::npos : slash - 7);
        url->path = slash == std::string::npos ? "/" : text.substr(slash);
        const size_t colon = host_port.rfind(':');
        if (colon != std::string::npos) {
            url->host = host_port.substr(0, colon);
            url->port = std::atoi(host_port.c_str() + colon + 1);
        }
        else {
            url->host = host_port;
            url->port = 80;
        }
        return !url->host.empty() && url->port > 0;
    }

    bool resolve(const std::string& host, int port, int type, sockaddr_in* out) {
        addrinfo hints{};
        hints.ai_family = AF_INET;
        hints.ai_socktype = type;
        addrinfo* found = nullptr;
        if (getaddrinfo(host.c_str(), std::to_string(port).c_str(), &hints, &found) != 0 || found == nullptr) {
            return false;
        }
        *out = *(sockaddr_in*)found->ai_addr;
        freeaddrinfo(found);
        return true;
    }

    // One HTTP exchange over TCP (the router's, on the local network): the whole answer, or empty.
    std::string http_exchange(const Url& url, const std::string& request) {
        sockaddr_in address{};
        if (!resolve(url.host, url.port, SOCK_STREAM, &address)) {
            return {};
        }
        SOCKET s = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
        if (s == INVALID_SOCKET) {
            return {};
        }
        DWORD timeout = 4000;
        setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, (const char*)&timeout, sizeof(timeout));
        setsockopt(s, SOL_SOCKET, SO_SNDTIMEO, (const char*)&timeout, sizeof(timeout));
        std::string answer;
        if (connect(s, (sockaddr*)&address, sizeof(address)) == 0) {
            size_t sent = 0;
            while (sent < request.size()) {
                const int n = send(s, request.data() + sent, (int)(request.size() - sent), 0);
                if (n <= 0) {
                    break;
                }
                sent += (size_t)n;
            }
            char buffer[4096];
            for (;;) {
                const int n = recv(s, buffer, sizeof(buffer), 0);
                if (n <= 0 || answer.size() > (1 << 20)) {
                    break;
                }
                answer.append(buffer, (size_t)n);
            }
        }
        closesocket(s);
        return answer;
    }

    int http_status(const std::string& answer) {
        const size_t space = answer.find(' ');
        return answer.rfind("HTTP/", 0) == 0 && space != std::string::npos ? std::atoi(answer.c_str() + space + 1) : 0;
    }

    std::string http_body(const std::string& answer) {
        const size_t split = answer.find("\r\n\r\n");
        if (split == std::string::npos) {
            return {};
        }
        std::string body = answer.substr(split + 4);
        if (lower(header(answer.substr(0, split + 2), "transfer-encoding")).find("chunked") == std::string::npos) {
            return body;
        }
        std::string plain;
        size_t at = 0;
        while (at < body.size()) {
            const size_t line_end = body.find("\r\n", at);
            if (line_end == std::string::npos) {
                break;
            }
            const size_t length = std::strtoul(body.substr(at, line_end - at).c_str(), nullptr, 16);
            if (length == 0) {
                break;
            }
            plain += body.substr(line_end + 2, length);
            at = line_end + 2 + length + 2;
        }
        return plain;
    }

    // SSDP: the description addresses (LOCATION) of the internet gateways that answer, within about 3 seconds
    std::vector<std::string> discover() {
        std::vector<std::string> locations;
        SOCKET s = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
        if (s == INVALID_SOCKET) {
            return locations;
        }
        sockaddr_in any{};
        any.sin_family = AF_INET;
        any.sin_addr.s_addr = htonl(INADDR_ANY);
        bind(s, (sockaddr*)&any, sizeof(any));
        DWORD ttl = 2;
        setsockopt(s, IPPROTO_IP, IP_MULTICAST_TTL, (const char*)&ttl, sizeof(ttl));
        {
            // out of the interface the internet is reached by (not a VPN's or a virtual machine's adapter): the address a UDP socket
            // "connected" to an internet address gets (nothing is sent)
            SOCKET probe = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
            sockaddr_in outside{};
            outside.sin_family = AF_INET;
            outside.sin_port = htons(53);
            inet_pton(AF_INET, "8.8.8.8", &outside.sin_addr);
            sockaddr_in mine{};
            int length = sizeof(mine);
            if (probe != INVALID_SOCKET && connect(probe, (sockaddr*)&outside, sizeof(outside)) == 0 && getsockname(probe, (sockaddr*)&mine, &length) == 0) {
                setsockopt(s, IPPROTO_IP, IP_MULTICAST_IF, (const char*)&mine.sin_addr, sizeof(mine.sin_addr));
            }
            if (probe != INVALID_SOCKET) {
                closesocket(probe);
            }
        }
        sockaddr_in group{};
        group.sin_family = AF_INET;
        group.sin_port = htons(1900);
        inet_pton(AF_INET, "239.255.255.250", &group.sin_addr);
        const char* const targets[] = {
            "urn:schemas-upnp-org:device:InternetGatewayDevice:1", "urn:schemas-upnp-org:device:InternetGatewayDevice:2",
            "urn:schemas-upnp-org:service:WANIPConnection:1", "urn:schemas-upnp-org:service:WANPPPConnection:1",
        };
        for (int round = 0; round < 2; round++) {
            for (const char* target : targets) {
                const std::string search = std::string("M-SEARCH * HTTP/1.1\r\nHOST: 239.255.255.250:1900\r\nST: ") + target +
                    "\r\nMAN: \"ssdp:discover\"\r\nMX: 2\r\n\r\n";
                sendto(s, search.data(), (int)search.size(), 0, (sockaddr*)&group, sizeof(group));
            }
        }
        const auto until = std::chrono::steady_clock::now() + std::chrono::milliseconds(3000);
        for (;;) {
            const auto left = std::chrono::duration_cast<std::chrono::milliseconds>(until - std::chrono::steady_clock::now()).count();
            if (left <= 0) {
                break;
            }
            fd_set read_set;
            FD_ZERO(&read_set);
            FD_SET(s, &read_set);
            timeval wait{ (long)(left / 1000), (long)((left % 1000) * 1000) };
            if (select(0, &read_set, nullptr, nullptr, &wait) <= 0) {
                break;
            }
            char buffer[2048];
            const int n = recvfrom(s, buffer, sizeof(buffer) - 1, 0, nullptr, nullptr);
            if (n <= 0) {
                continue;
            }
            const std::string location = header(std::string(buffer, (size_t)n), "location");
            if (!location.empty() && std::find(locations.begin(), locations.end(), location) == locations.end()) {
                locations.push_back(location);
            }
        }
        closesocket(s);
        return locations;
    }

    struct Gateway {
        Url control;
        std::string service;
    };

    // the gateway's WANIPConnection (or WANPPPConnection) service, from its description
    bool find_service(const std::string& location, Gateway* gateway) {
        Url url;
        if (!parse_url(location, &url)) {
            return false;
        }
        const std::string answer = http_exchange(url, "GET " + url.path + " HTTP/1.1\r\nHost: " + url.host + ":" + std::to_string(url.port) +
            "\r\nConnection: close\r\n\r\n");
        if (http_status(answer) != 200) {
            return false;
        }
        const std::string xml = http_body(answer);
        Url base = url;
        const std::string url_base = xml_value(xml, "URLBase");
        if (!url_base.empty()) {
            parse_url(url_base, &base);
        }
        bool found = false;
        size_t at = 0;
        for (;;) {
            size_t type_at = 0;
            const std::string type = xml_value(xml, "serviceType", at, &type_at);
            if (type.empty()) {
                break;
            }
            at = type_at + 12;
            const bool ip = type.find(":WANIPConnection:") != std::string::npos;
            const bool ppp = type.find(":WANPPPConnection:") != std::string::npos;
            if (!ip && !ppp) {
                continue;
            }
            const std::string control = xml_value(xml, "controlURL", type_at);
            if (control.empty()) {
                continue;
            }
            Gateway candidate;
            candidate.service = type;
            if (lower(control.substr(0, 7)) == "http://") {
                if (!parse_url(control, &candidate.control)) {
                    continue;
                }
            }
            else {
                candidate.control = base;
                candidate.control.path = control[0] == '/' ? control : "/" + control;
            }
            *gateway = candidate;
            found = true;
            if (ip) {
                break;   // (WANIPConnection first)
            }
        }
        return found;
    }

    // a SOAP action on the service: the HTTP status, and the answer's body
    std::string soap(const Gateway& gateway, const std::string& action, const std::string& arguments, int* status) {
        const std::string body = "<?xml version=\"1.0\"?>\r\n<s:Envelope xmlns:s=\"http://schemas.xmlsoap.org/soap/envelope/\" "
            "s:encodingStyle=\"http://schemas.xmlsoap.org/soap/encoding/\"><s:Body><u:" + action + " xmlns:u=\"" + gateway.service + "\">" +
            arguments + "</u:" + action + "></s:Body></s:Envelope>\r\n";
        const Url& url = gateway.control;
        const std::string request = "POST " + url.path + " HTTP/1.1\r\nHost: " + url.host + ":" + std::to_string(url.port) +
            "\r\nContent-Type: text/xml; charset=\"utf-8\"\r\nSOAPAction: \"" + gateway.service + "#" + action + "\"\r\nContent-Length: " +
            std::to_string(body.size()) + "\r\nConnection: close\r\n\r\n" + body;
        const std::string answer = http_exchange(url, request);
        *status = http_status(answer);
        return http_body(answer);
    }

    // this machine's address on the network the router is on
    std::string local_address(const Url& router) {
        sockaddr_in address{};
        if (!resolve(router.host, router.port, SOCK_DGRAM, &address)) {
            return {};
        }
        SOCKET s = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
        if (s == INVALID_SOCKET) {
            return {};
        }
        std::string text;
        if (connect(s, (sockaddr*)&address, sizeof(address)) == 0) {
            sockaddr_in mine{};
            int length = sizeof(mine);
            if (getsockname(s, (sockaddr*)&mine, &length) == 0) {
                char buffer[INET_ADDRSTRLEN] = {};
                inet_ntop(AF_INET, &mine.sin_addr, buffer, sizeof(buffer));
                text = buffer;
            }
        }
        closesocket(s);
        return text;
    }

    // not an internet address: the router is behind another NAT (the internet provider's, or another router)
    bool behind_another_nat(const std::string& address) {
        unsigned a = 0, b = 0, c = 0, d = 0;
        if (std::sscanf(address.c_str(), "%u.%u.%u.%u", &a, &b, &c, &d) != 4) {
            return false;
        }
        return a == 10 || a == 0 || a == 127 || (a == 172 && b >= 16 && b <= 31) || (a == 192 && b == 168) || (a == 100 && b >= 64 && b <= 127) ||
            (a == 169 && b == 254);
    }

    std::string mapping_arguments(const std::string& client, int lease) {
        return "<NewRemoteHost></NewRemoteHost><NewExternalPort>" + std::to_string(game_port) + "</NewExternalPort><NewProtocol>UDP</NewProtocol>"
            "<NewInternalPort>" + std::to_string(game_port) + "</NewInternalPort><NewInternalClient>" + client + "</NewInternalClient>"
            "<NewEnabled>1</NewEnabled><NewPortMappingDescription>Conker online (CBFD-Online)</NewPortMappingDescription>"
            "<NewLeaseDuration>" + std::to_string(lease) + "</NewLeaseDuration>";
    }

    // AddPortMapping: 0 done, else the UPnP error code (or the HTTP status)
    int add_mapping(const Gateway& gateway, const std::string& client, int lease) {
        int status = 0;
        const std::string body = soap(gateway, "AddPortMapping", mapping_arguments(client, lease), &status);
        if (status == 200) {
            return 0;
        }
        const std::string code = xml_value(body, "errorCode");
        return code.empty() ? (status == 0 ? -1 : status) : std::atoi(code.c_str());
    }

    void run() {
        WSADATA wsa;
        WSAStartup(MAKEWORD(2, 2), &wsa);
        set_status(0, "");
        Gateway gateway;
        bool found = false;
        for (const std::string& location : discover()) {
            std::printf("[upnp] gateway description: %s\n", location.c_str());
            if (find_service(location, &gateway)) {
                found = true;
                break;
            }
        }
        if (!found) {
            set_status(3, "NO UPNP ROUTER");
            return;
        }
        std::printf("[upnp] %s at %s:%d%s\n", gateway.service.c_str(), gateway.control.host.c_str(), gateway.control.port, gateway.control.path.c_str());
        int status = 0;
        const std::string external = xml_value(soap(gateway, "GetExternalIPAddress", "", &status), "NewExternalIPAddress");
        const std::string client = local_address(gateway.control);
        std::printf("[upnp] the router's internet address: %s, this machine: %s\n", external.empty() ? "(not given)" : external.c_str(), client.c_str());
        if (!external.empty() && behind_another_nat(external)) {
            set_status(2, "CGNAT / DOUBLE NAT");
            return;
        }
        if (client.empty()) {
            set_status(3, "NO LOCAL ADDRESS");
            return;
        }
        const char* no_upnp = SDL_getenv("CONKER_NO_UPNP");
        const bool map = no_upnp == nullptr || *no_upnp == 0 || *no_upnp == '0';
        int lease = 7200;
        int added = -1;
        if (map) {
            added = add_mapping(gateway, client, lease);
            if (added == 725) {
                // OnlyPermanentLeasesSupported
                lease = 0;
                added = add_mapping(gateway, client, lease);
            }
            std::printf("[upnp] AddPortMapping UDP %d -> %s: %s\n", game_port, client.c_str(), added == 0 ? "done" : ("error " + std::to_string(added)).c_str());
        }
        // what the router has for the port now
        const std::string entry = soap(gateway, "GetSpecificPortMappingEntry",
            "<NewRemoteHost></NewRemoteHost><NewExternalPort>" + std::to_string(game_port) + "</NewExternalPort><NewProtocol>UDP</NewProtocol>", &status);
        const std::string mapped_to = status == 200 ? xml_value(entry, "NewInternalClient") : std::string();
        const std::string mapped_port = status == 200 ? xml_value(entry, "NewInternalPort") : std::string();
        std::printf("[upnp] UDP %d is mapped to: %s%s%s\n", game_port, mapped_to.empty() ? "(nothing)" : mapped_to.c_str(),
            mapped_port.empty() ? "" : ":", mapped_port.c_str());
        if (!mapped_to.empty() && mapped_to == client && std::atoi(mapped_port.c_str()) == game_port) {
            set_status(1, map && added == 0 ? "UPNP" : "FORWARDED");
        }
        else if (!mapped_to.empty() && mapped_to != client) {
            set_status(2, "TAKEN BY " + mapped_to);
            return;
        }
        else if (map && added != 0) {
            set_status(added == 718 ? 3 : 2, added == 718 ? "IN USE" : "UPNP REFUSED");
            return;
        }
        else {
            set_status(3, map ? "NOT CONFIRMED" : "NOT MAPPED");
            return;
        }
        // renew the lease while the program runs (it ends by itself when it doesn't)
        while (map && added == 0 && lease > 0) {
            std::this_thread::sleep_for(std::chrono::minutes(30));
            add_mapping(gateway, client, lease);
        }
    }
#endif
}

void conker::upnp::start() {
    if (started.exchange(true)) {
        return;
    }
#if defined(_WIN32)
    std::thread(run).detach();
#else
    set_status(3, "NOT CHECKED HERE");
#endif
}

conker::upnp::Status conker::upnp::status() {
    std::lock_guard lock{ status_mutex };
    return current;
}

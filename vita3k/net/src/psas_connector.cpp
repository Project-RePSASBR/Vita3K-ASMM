// Vita3K emulator project
// Copyright (C) 2026 Vita3K team
//
// This program is free software; you can redistribute it and/or modify
// it under the terms of the GNU General Public License as published by
// the Free Software Foundation; either version 2 of the License, or
// (at your option) any later version.
//
// This program is distributed in the hope that it will be useful,
// but WITHOUT ANY WARRANTY; without even the implied warranty of
// MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
// GNU General Public License for more details.
//
// You should have received a copy of the GNU General Public License along
// with this program; if not, write to the Free Software Foundation, Inc.,
// 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA.

#include <net/psas_connector.h>

#include <config/state.h>
#include <config/version.h>
#include <emuenv/state.h>
#include <io/state.h>
#include <net/functions.h>
#include <net/state.h>
#include <util/log.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <cstring>
#include <iterator>

#ifdef _WIN32
#ifndef SIO_UDP_CONNRESET
#define SIO_UDP_CONNRESET _WSAIOW(IOC_VENDOR, 12)
#endif
static constexpr abs_socket INVALID_HELLO_SOCKET = INVALID_SOCKET;
#else
static constexpr abs_socket INVALID_HELLO_SOCKET = -1;
#endif

// The only titles gated for console P2P and connector mode
static constexpr std::array<std::string_view, 4> psas_retail_titles = {
    "PCSF00153", // PlayStation All-Stars Battle Royale (EU)
    "PCSA00069", // PlayStation All-Stars Battle Royale (US)
    "PCSC00032", // PlayStation All-Stars Battle Royale (JP)
    "PCSD00040", // PlayStation All-Stars Battle Royale (Asia)
};

bool psas_is_retail_title(std::string_view title_id) {
    return std::find(psas_retail_titles.begin(), psas_retail_titles.end(), title_id) != psas_retail_titles.end();
}

bool psas_connector_mode_active(std::string_view title_id, bool connector_mode_setting) {
    return connector_mode_setting && psas_is_retail_title(title_id);
}

bool psas_connector_mode_active(const EmuEnvState &emuenv) {
    return psas_connector_mode_active(emuenv.io.title_id, emuenv.cfg.psas_connector_mode);
}

// Appends a u8 length and at most max_len bytes of the string, without splitting a UTF-8 sequence
static void append_string(std::vector<uint8_t> &out, std::string_view str, size_t max_len) {
    size_t len = std::min(str.size(), max_len);
    if (len < str.size()) {
        while ((len > 0) && ((static_cast<uint8_t>(str[len]) & 0xC0) == 0x80))
            --len;
    }

    out.push_back(static_cast<uint8_t>(len));
    out.insert(out.end(), str.begin(), str.begin() + len);
}

std::vector<uint8_t> psas_encode_hello(const PsasHello &hello) {
    std::vector<uint8_t> out;
    out.reserve(PSAS_HELLO_MIN_SIZE + PSAS_HELLO_MAX_FORK_VERSION + PSAS_HELLO_MAX_TITLE_ID + hello.bound_address.size());
    out.insert(out.end(), std::begin(PSAS_HELLO_MAGIC), std::end(PSAS_HELLO_MAGIC));
    out.push_back(hello.version);
    out.push_back(hello.emulator);
    out.push_back(hello.flags);
    out.push_back(0); // reserved
    out.push_back(static_cast<uint8_t>(hello.interval_ms & 0xFF));
    out.push_back(static_cast<uint8_t>(hello.interval_ms >> 8));
    append_string(out, hello.fork_version, PSAS_HELLO_MAX_FORK_VERSION);
    append_string(out, hello.title_id, PSAS_HELLO_MAX_TITLE_ID);
    append_string(out, hello.bound_address, PSAS_HELLO_MAX_ADDRESS);
    return out;
}

std::string psas_fork_version(std::string_view app_version, std::string_view app_hash) {
    std::string version{ PSAS_FORK_NAME };
    version += " r";
    version += std::to_string(PSAS_FORK_REVISION);
    version += ' ';
    version += app_version;
    if (!app_hash.empty()) {
        version += '-';
        version += app_hash;
    }
    return version;
}

// s_addr is not usable on native in_addr on Windows (socket.h undefines the macro), so copy the raw bytes
static uint32_t get_native_ip(const sockaddr_in &addr) {
    uint32_t ip;
    std::memcpy(&ip, &addr.sin_addr, sizeof(ip));
    return ip;
}

static std::string ip_to_string(uint32_t ip) {
    char str[INET_ADDRSTRLEN] = {};
    inet_ntop(AF_INET, &ip, str, sizeof(str));
    return str;
}

static std::string endpoint_to_string(const sockaddr_in &endpoint) {
    return ip_to_string(get_native_ip(endpoint)) + ":" + std::to_string(ntohs(endpoint.sin_port));
}

static bool is_loopback(uint32_t ip) {
    return (ntohl(ip) >> 24) == 127;
}

static sockaddr_in connector_endpoint() {
    sockaddr_in endpoint{};
    endpoint.sin_family = AF_INET;
    endpoint.sin_port = htons(PSAS_CONNECTOR_PORT);
    const uint32_t ip = htonl(INADDR_LOOPBACK);
    std::memcpy(&endpoint.sin_addr, &ip, sizeof(ip));
    return endpoint;
}

static bool same_endpoint(const sockaddr_in &lhs, const sockaddr_in &rhs) {
    return (lhs.sin_port == rhs.sin_port) && (get_native_ip(lhs) == get_native_ip(rhs));
}

PsasP2PSettings psas_resolve_p2p_settings(std::string_view title_id, bool connector_mode_setting, const std::string &bind_address, const std::string &broadcast_forward) {
    PsasP2PSettings settings;
    settings.title_gated = psas_is_retail_title(title_id);
    settings.connector_mode = psas_connector_mode_active(title_id, connector_mode_setting);
    if (!settings.connector_mode) {
        settings.bind_addr = P2PContext::parse_bind_address(bind_address);
        settings.broadcast_forward = P2PContext::parse_forward_list(broadcast_forward);
        return settings;
    }

    // Loopback bind whatever is configured, and the connector first in the forward list (configured endpoints are kept)
    settings.bind_addr = htonl(INADDR_LOOPBACK);
    settings.broadcast_forward.push_back(connector_endpoint());
    for (const auto &endpoint : P2PContext::parse_forward_list(broadcast_forward)) {
        const auto duplicate = [&endpoint](const sockaddr_in &other) { return same_endpoint(endpoint, other); };
        if (std::none_of(settings.broadcast_forward.begin(), settings.broadcast_forward.end(), duplicate))
            settings.broadcast_forward.push_back(endpoint);
    }

    return settings;
}

PsasP2PSettings psas_resolve_p2p_settings(const EmuEnvState &emuenv) {
    return psas_resolve_p2p_settings(emuenv.io.title_id, emuenv.cfg.psas_connector_mode, emuenv.cfg.p2p_bind_address, emuenv.cfg.p2p_broadcast_forward);
}

void psas_start_hello(EmuEnvState &emuenv) {
    emuenv.net.psas_hello.reset();
    if (!psas_is_retail_title(emuenv.io.title_id))
        return;

    emuenv.net.psas_hello = std::make_unique<PsasHelloSender>(psas_resolve_p2p_settings(emuenv), !emuenv.cfg.psas_connector_mode, psas_fork_version(app_version, app_hash), emuenv.io.title_id);
    if (emuenv.net.p2p)
        emuenv.net.psas_hello->watch(emuenv.net.p2p);
}

PsasHelloSender::PsasHelloSender(const PsasP2PSettings &p2p_settings, bool user_opted_out, std::string fork_version, std::string title_id)
    : settings(p2p_settings)
    , opted_out(user_opted_out)
    , targets(p2p_settings.broadcast_forward) {
    if (targets.empty())
        targets.push_back(connector_endpoint());

    hello.fork_version = std::move(fork_version);
    hello.title_id = std::move(title_id);

    std::string target_list;
    for (const auto &target : targets)
        target_list += (target_list.empty() ? "" : ", ") + endpoint_to_string(target);
    LOG_INFO("ASBR connector hello: fork '{}', title {}, sent to {} every {} ms (connector mode {})", hello.fork_version, hello.title_id, target_list, PSAS_HELLO_INTERVAL_MS, settings.connector_mode ? "on" : "off");

    thread = std::thread(&PsasHelloSender::run, this);
}

PsasHelloSender::~PsasHelloSender() {
    stop();
}

void PsasHelloSender::stop() {
    {
        const std::lock_guard<std::mutex> lock(mutex);
        stopping = true;
    }
    cond.notify_all();
    if (thread.joinable())
        thread.join();
}

void PsasHelloSender::watch(const P2PContextPtr &ctx) {
    const std::lock_guard<std::mutex> lock(mutex);
    p2p = ctx;
}

std::vector<uint8_t> PsasHelloSender::make_datagram() {
    P2PContextPtr ctx;
    {
        const std::lock_guard<std::mutex> lock(mutex);
        ctx = p2p.lock();
    }

    uint32_t bound_addr = 0;
    const bool bound = ctx && ctx->get_bound_addr(SCE_NET_ADHOC_PORT, bound_addr);

    // The actual bind once the game opened its P2P port, the one it is going to get before that
    hello.flags = 0;
    if (is_loopback(bound ? bound_addr : settings.bind_addr))
        hello.flags |= PSAS_HELLO_FLAG_LOOPBACK_BIND;
    if (!settings.broadcast_forward.empty())
        hello.flags |= PSAS_HELLO_FLAG_BROADCAST_FORWARD;
    if (opted_out)
        hello.flags |= PSAS_HELLO_FLAG_OPTED_OUT;
    if (settings.title_gated)
        hello.flags |= PSAS_HELLO_FLAG_TITLE_GATED;

    hello.bound_address = (bound && settings.connector_mode) ? ip_to_string(bound_addr) + ":" + std::to_string(SCE_NET_ADHOC_PORT) : std::string{};
    return psas_encode_hello(hello);
}

static abs_socket open_hello_socket() {
    const abs_socket sock = ::socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
#ifdef _WIN32
    if (sock != INVALID_HELLO_SOCKET) {
        // Otherwise a hello bounced with ICMP port-unreachable (no connector running) fails the next send
        BOOL report_connreset = FALSE;
        DWORD bytes_returned = 0;
        WSAIoctl(sock, SIO_UDP_CONNRESET, &report_connreset, sizeof(report_connreset), nullptr, 0, &bytes_returned, nullptr, nullptr);
    }
#endif
    return sock;
}

static void close_hello_socket(abs_socket sock) {
#ifdef _WIN32
    closesocket(sock);
#else
    ::close(sock);
#endif
}

static int last_socket_error() {
#ifdef _WIN32
    return WSAGetLastError();
#else
    return errno;
#endif
}

void PsasHelloSender::run() {
#ifdef _WIN32
    // Holds a Winsock reference of its own: the hello starts before the game initialises the network
    WSADATA wsa_data;
    const bool wsa_started = WSAStartup(MAKEWORD(2, 2), &wsa_data) == 0;
#endif
    abs_socket sock = INVALID_HELLO_SOCKET;

    std::unique_lock<std::mutex> lock(mutex);
    while (!stopping) {
        lock.unlock();

        if (sock == INVALID_HELLO_SOCKET) {
            sock = open_hello_socket();
            if ((sock == INVALID_HELLO_SOCKET) && !error_logged) {
                error_logged = true;
                LOG_WARN("ASBR connector hello: failed to create the socket (error {}), further failures are not logged", last_socket_error());
            }
        }

        if (sock != INVALID_HELLO_SOCKET) {
            const auto datagram = make_datagram();
            for (const auto &target : targets) {
                const auto res = sendto(sock, reinterpret_cast<const char *>(datagram.data()), static_cast<int>(datagram.size()), 0, reinterpret_cast<const sockaddr *>(&target), sizeof(target));
                if ((res < 0) && !error_logged) {
                    error_logged = true;
                    LOG_WARN("ASBR connector hello: send to {} failed (error {}), further failures are not logged", endpoint_to_string(target), last_socket_error());
                }
            }
        }

        lock.lock();
        cond.wait_for(lock, std::chrono::milliseconds(PSAS_HELLO_INTERVAL_MS), [this] { return stopping; });
    }
    lock.unlock();

    if (sock != INVALID_HELLO_SOCKET)
        close_hello_socket(sock);
#ifdef _WIN32
    if (wsa_started)
        WSACleanup();
#endif
}

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

#pragma once

#include <net/p2pmux.h>

#include <condition_variable>
#include <cstdint>
#include <memory>
#include <mutex>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

// ASBR connector mode.
// For the retail PlayStation All-Stars Battle Royale titles the console P2P socket is pinned to loopback
// and every broadcast is also forwarded to the All-Stars Matchmaker connector running on this PC,
// whatever the stored p2p-* settings say (the user can opt out with the psas-connector-mode setting).
// While such a title runs, a small hello datagram tells the connector that this emulator is there.

struct EmuEnvState;

// Identity of this fork in the hello. app_version/app_hash are upstream's (the fork has no commits of its own),
// so the name and revision are what tell the connector which fork build it is talking to.
constexpr std::string_view PSAS_FORK_NAME = "psasbr3k";
// Bump by hand whenever the fork's connector-relevant behaviour changes (bind/forward rules, the hello, the P2P framing)
constexpr int PSAS_FORK_REVISION = 1;

constexpr uint16_t PSAS_CONNECTOR_PORT = 4000;
constexpr uint16_t PSAS_HELLO_INTERVAL_MS = 2000;

// Hello datagram (fork -> connector), little endian:
// magic "RPSB", u8 version, u8 emulator, u8 flags, u8 reserved, u16 interval in ms,
// then three u8-length-prefixed strings: fork version, title id, bound game address ("ip:port" or empty)
constexpr uint8_t PSAS_HELLO_MAGIC[4] = { 'R', 'P', 'S', 'B' };
constexpr uint8_t PSAS_HELLO_VERSION = 1;
constexpr uint8_t PSAS_HELLO_EMULATOR_RPCS3 = 1;
constexpr uint8_t PSAS_HELLO_EMULATOR_VITA3K = 2;
constexpr uint8_t PSAS_HELLO_FLAG_LOOPBACK_BIND = 1 << 0; // the P2P socket binds (or is bound) to a loopback address
constexpr uint8_t PSAS_HELLO_FLAG_BROADCAST_FORWARD = 1 << 1; // broadcasts are forwarded to at least one endpoint
constexpr uint8_t PSAS_HELLO_FLAG_OPTED_OUT = 1 << 2; // the user disabled connector mode
constexpr uint8_t PSAS_HELLO_FLAG_TITLE_GATED = 1 << 3; // the running title is a retail PSASBR id
constexpr size_t PSAS_HELLO_HEADER_SIZE = 10;
constexpr size_t PSAS_HELLO_MIN_SIZE = PSAS_HELLO_HEADER_SIZE + 3;
constexpr size_t PSAS_HELLO_MAX_FORK_VERSION = 40;
constexpr size_t PSAS_HELLO_MAX_TITLE_ID = 9;
constexpr size_t PSAS_HELLO_MAX_ADDRESS = 255;

struct PsasHello {
    uint8_t version = PSAS_HELLO_VERSION;
    uint8_t emulator = PSAS_HELLO_EMULATOR_VITA3K;
    uint8_t flags = 0;
    uint16_t interval_ms = PSAS_HELLO_INTERVAL_MS;
    std::string fork_version; // truncated to PSAS_HELLO_MAX_FORK_VERSION bytes
    std::string title_id; // truncated to PSAS_HELLO_MAX_TITLE_ID bytes
    std::string bound_address; // "ip:port", empty while the game's P2P port is not bound
};

std::vector<uint8_t> psas_encode_hello(const PsasHello &hello);

// Hello fork version: "<fork name> r<fork revision> <app version>[-<app hash>]", e.g. "psasbr3k r1 v0.2.1-610e6970".
// Name and revision come first so they survive the encoder's 40-byte clamp.
std::string psas_fork_version(std::string_view app_version, std::string_view app_hash);

// Retail PlayStation All-Stars Battle Royale title ids (EU, US, JP, Asia). Betas and demos are deliberately not listed.
bool psas_is_retail_title(std::string_view title_id);
// Connector mode applies: the title is a retail PSASBR id and the user did not opt out
bool psas_connector_mode_active(std::string_view title_id, bool connector_mode_setting);
bool psas_connector_mode_active(const EmuEnvState &emuenv);

// Console P2P settings in effect for a title, with connector mode applied
struct PsasP2PSettings {
    bool title_gated = false;
    bool connector_mode = false;
    uint32_t bind_addr = 0; // network order, 0 = INADDR_ANY
    std::vector<sockaddr_in> broadcast_forward;
};

PsasP2PSettings psas_resolve_p2p_settings(std::string_view title_id, bool connector_mode_setting, const std::string &bind_address, const std::string &broadcast_forward);
PsasP2PSettings psas_resolve_p2p_settings(const EmuEnvState &emuenv);

// Sends the hello every PSAS_HELLO_INTERVAL_MS from its own host UDP socket (not a guest socket)
// to every broadcast forward target, or to 127.0.0.1:4000 when there is none, until stopped.
// psas_start_hello (net/functions.h) creates one per title run in NetState::psas_hello.
class PsasHelloSender {
public:
    PsasHelloSender(const PsasP2PSettings &p2p_settings, bool user_opted_out, std::string fork_version, std::string title_id);
    ~PsasHelloSender();
    PsasHelloSender(const PsasHelloSender &) = delete;
    PsasHelloSender &operator=(const PsasHelloSender &) = delete;

    // Reports the game's P2P address in the hello once this context opens the shared port
    void watch(const P2PContextPtr &ctx);
    void stop();

private:
    const PsasP2PSettings settings;
    const bool opted_out;
    std::vector<sockaddr_in> targets;
    PsasHello hello;

    std::mutex mutex;
    std::condition_variable cond;
    bool stopping = false;
    std::weak_ptr<P2PContext> p2p;
    bool error_logged = false;
    std::thread thread;

    void run();
    std::vector<uint8_t> make_datagram();
};

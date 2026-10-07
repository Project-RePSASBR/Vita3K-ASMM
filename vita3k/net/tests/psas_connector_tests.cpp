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

#include <gtest/gtest.h>

#include <chrono>
#include <cstring>
#include <string>
#include <vector>

static uint32_t ipv4(const char *str) {
    uint32_t ip = 0;
    inet_pton(AF_INET, str, &ip);
    return ip;
}

static uint32_t endpoint_ip(const sockaddr_in &endpoint) {
    uint32_t ip;
    std::memcpy(&ip, &endpoint.sin_addr, sizeof(ip));
    return ip;
}

TEST(psas_hello, layout_matches_design_note) {
    PsasHello hello;
    hello.flags = PSAS_HELLO_FLAG_LOOPBACK_BIND | PSAS_HELLO_FLAG_BROADCAST_FORWARD | PSAS_HELLO_FLAG_TITLE_GATED;
    hello.fork_version = "psasbr3k r1 v0.2.1-abc1234";
    hello.title_id = "PCSF00153";
    hello.bound_address = "127.0.0.1:3658";

    const std::vector<uint8_t> expected = {
        0x52, 0x50, 0x53, 0x42, // magic "RPSB"
        0x01, // hello version
        0x02, // emulator: Vita3K
        0x0B, // flags: loopback bind, broadcast forward, title gated
        0x00, // reserved
        0xD0, 0x07, // interval 2000 ms, little endian
        0x1A, 'p', 's', 'a', 's', 'b', 'r', '3', 'k', ' ', 'r', '1', ' ', // fork version: name, revision,
        'v', '0', '.', '2', '.', '1', '-', 'a', 'b', 'c', '1', '2', '3', '4', // app version and hash
        0x09, 'P', 'C', 'S', 'F', '0', '0', '1', '5', '3', // title id
        0x0E, '1', '2', '7', '.', '0', '.', '0', '.', '1', ':', '3', '6', '5', '8', // bound game address
    };
    EXPECT_EQ(psas_encode_hello(hello), expected);
}

TEST(psas_hello, empty_strings_give_the_minimum_length) {
    PsasHello hello;
    hello.flags = PSAS_HELLO_FLAG_OPTED_OUT | PSAS_HELLO_FLAG_TITLE_GATED;

    const std::vector<uint8_t> expected = {
        0x52, 0x50, 0x53, 0x42, 0x01, 0x02, 0x0C, 0x00, 0xD0, 0x07, 0x00, 0x00, 0x00
    };
    const auto datagram = psas_encode_hello(hello);
    EXPECT_EQ(datagram.size(), PSAS_HELLO_MIN_SIZE);
    EXPECT_EQ(datagram.size(), 13u);
    EXPECT_EQ(datagram, expected);
}

TEST(psas_hello, truncates_fork_version_and_title_id) {
    PsasHello hello;
    hello.fork_version = std::string(50, 'x');
    hello.title_id = "PCSF00153-EXTRA";

    const auto datagram = psas_encode_hello(hello);
    ASSERT_EQ(datagram.size(), 10u + 1 + 40 + 1 + 9 + 1);
    EXPECT_EQ(datagram[10], 40);
    EXPECT_EQ(std::string(datagram.begin() + 11, datagram.begin() + 51), std::string(40, 'x'));
    EXPECT_EQ(datagram[51], 9);
    EXPECT_EQ(std::string(datagram.begin() + 52, datagram.begin() + 61), "PCSF00153");
    EXPECT_EQ(datagram[61], 0);
}

TEST(psas_hello, truncation_does_not_split_utf8_sequences) {
    PsasHello hello;
    // 39 bytes, then a two-byte U+00B7 that would straddle the 40-byte limit
    hello.fork_version = std::string(39, 'a') + "\xC2\xB7" + "zz";

    const auto datagram = psas_encode_hello(hello);
    EXPECT_EQ(datagram[10], 39);
    EXPECT_EQ(datagram.size(), 10u + 1 + 39 + 1 + 1);

    hello.fork_version = std::string(38, 'a') + "\xC2\xB7" + "zz";
    EXPECT_EQ(psas_encode_hello(hello)[10], 40);
}

TEST(psas_fork_version, name_and_revision_come_first) {
    EXPECT_EQ(PSAS_FORK_NAME, "psasbr3k");
    const std::string prefix = "psasbr3k r" + std::to_string(PSAS_FORK_REVISION) + " ";
    EXPECT_EQ(psas_fork_version("v0.2.1", "610e6970"), prefix + "v0.2.1-610e6970");
    EXPECT_EQ(psas_fork_version("v0.2.1", ""), prefix + "v0.2.1");
}

TEST(psas_fork_version, name_and_revision_survive_the_clamp) {
    PsasHello hello;
    hello.fork_version = psas_fork_version(std::string(60, 'v'), "610e6970");

    const auto datagram = psas_encode_hello(hello);
    ASSERT_EQ(datagram[10], PSAS_HELLO_MAX_FORK_VERSION);
    const std::string sent(datagram.begin() + 11, datagram.begin() + 11 + PSAS_HELLO_MAX_FORK_VERSION);
    const std::string prefix = "psasbr3k r" + std::to_string(PSAS_FORK_REVISION) + " ";
    EXPECT_EQ(sent.substr(0, prefix.size()), prefix);
}

TEST(psas_gate, retail_titles_only) {
    EXPECT_TRUE(psas_is_retail_title("PCSF00153"));
    EXPECT_TRUE(psas_is_retail_title("PCSA00069"));
    EXPECT_TRUE(psas_is_retail_title("PCSC00032"));
    EXPECT_TRUE(psas_is_retail_title("PCSD00040"));

    EXPECT_FALSE(psas_is_retail_title(""));
    EXPECT_FALSE(psas_is_retail_title("pcsf00153"));
    EXPECT_FALSE(psas_is_retail_title("PCSF001530"));
    EXPECT_FALSE(psas_is_retail_title("PCSA00084")); // US beta
    EXPECT_FALSE(psas_is_retail_title("PCSF00192")); // EU beta
    EXPECT_FALSE(psas_is_retail_title("PCSG00095")); // a different game

    EXPECT_TRUE(psas_connector_mode_active("PCSC00032", true));
    EXPECT_FALSE(psas_connector_mode_active("PCSC00032", false));
    EXPECT_FALSE(psas_connector_mode_active("PCSE00001", true));
}

TEST(psas_settings, connector_mode_forces_loopback_bind_and_forward) {
    const auto settings = psas_resolve_p2p_settings("PCSA00069", true, "192.168.1.5", "10.0.0.2:5000, 127.0.0.1:4000");
    EXPECT_TRUE(settings.title_gated);
    EXPECT_TRUE(settings.connector_mode);
    EXPECT_EQ(settings.bind_addr, ipv4("127.0.0.1"));

    // The connector comes first, configured endpoints are kept and duplicates dropped
    ASSERT_EQ(settings.broadcast_forward.size(), 2u);
    EXPECT_EQ(endpoint_ip(settings.broadcast_forward[0]), ipv4("127.0.0.1"));
    EXPECT_EQ(ntohs(settings.broadcast_forward[0].sin_port), PSAS_CONNECTOR_PORT);
    EXPECT_EQ(endpoint_ip(settings.broadcast_forward[1]), ipv4("10.0.0.2"));
    EXPECT_EQ(ntohs(settings.broadcast_forward[1].sin_port), 5000);
}

TEST(psas_settings, opt_out_and_other_titles_keep_the_stored_settings) {
    const auto opted_out = psas_resolve_p2p_settings("PCSF00153", false, "192.168.1.5", "");
    EXPECT_TRUE(opted_out.title_gated);
    EXPECT_FALSE(opted_out.connector_mode);
    EXPECT_EQ(opted_out.bind_addr, ipv4("192.168.1.5"));
    EXPECT_TRUE(opted_out.broadcast_forward.empty());

    const auto other = psas_resolve_p2p_settings("PCSE00001", true, "", "10.0.0.2:5000");
    EXPECT_FALSE(other.title_gated);
    EXPECT_FALSE(other.connector_mode);
    EXPECT_EQ(other.bind_addr, 0u);
    ASSERT_EQ(other.broadcast_forward.size(), 1u);
    EXPECT_EQ(ntohs(other.broadcast_forward[0].sin_port), 5000);
}

#ifdef _WIN32
struct WinsockGuard {
    WinsockGuard() {
        WSADATA data;
        WSAStartup(MAKEWORD(2, 2), &data);
    }
    ~WinsockGuard() {
        WSACleanup();
    }
};
#endif

static void close_socket(abs_socket sock) {
#ifdef _WIN32
    closesocket(sock);
#else
    ::close(sock);
#endif
}

TEST(psas_hello_sender, sends_to_the_forward_targets_and_stops_promptly) {
#ifdef _WIN32
    WinsockGuard winsock;
#endif
    const abs_socket listener = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    inet_pton(AF_INET, "127.0.0.1", &addr.sin_addr);
    ASSERT_EQ(bind(listener, reinterpret_cast<const sockaddr *>(&addr), sizeof(addr)), 0);
    socklen_t addr_len = sizeof(addr);
    getsockname(listener, reinterpret_cast<sockaddr *>(&addr), &addr_len);

    PsasP2PSettings settings;
    settings.title_gated = true;
    settings.connector_mode = true;
    settings.bind_addr = ipv4("127.0.0.1");
    settings.broadcast_forward.push_back(addr);

    auto sender = std::make_unique<PsasHelloSender>(settings, false, psas_fork_version("v0.2.1", "test"), "PCSD00040");

    // The first hello goes out as soon as the sender starts
    fd_set fds;
    FD_ZERO(&fds);
    FD_SET(listener, &fds);
    timeval timeout{ 3, 0 };
    ASSERT_EQ(select(static_cast<int>(listener) + 1, &fds, nullptr, nullptr, &timeout), 1);

    char buf[256];
    const auto received = recv(listener, buf, sizeof(buf), 0);
    const PsasHello expected_hello{
        .flags = PSAS_HELLO_FLAG_LOOPBACK_BIND | PSAS_HELLO_FLAG_BROADCAST_FORWARD | PSAS_HELLO_FLAG_TITLE_GATED,
        .fork_version = psas_fork_version("v0.2.1", "test"),
        .title_id = "PCSD00040",
    };
    const auto expected = psas_encode_hello(expected_hello);
    ASSERT_EQ(received, static_cast<decltype(received)>(expected.size()));
    EXPECT_EQ(std::vector<uint8_t>(buf, buf + received), expected);

    // Stopping wakes the sender up instead of waiting for the next interval
    const auto start = std::chrono::steady_clock::now();
    sender.reset();
    EXPECT_LT(std::chrono::steady_clock::now() - start, std::chrono::milliseconds(PSAS_HELLO_INTERVAL_MS / 2));

    close_socket(listener);
}

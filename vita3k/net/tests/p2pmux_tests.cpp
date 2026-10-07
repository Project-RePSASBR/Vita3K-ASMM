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

#include <net/p2pmux.h>

#include <gtest/gtest.h>

#include <chrono>
#include <cstring>
#include <future>

#ifdef _WIN32
struct WinsockEnvironment : public ::testing::Environment {
    void SetUp() override {
        WSADATA data;
        WSAStartup(MAKEWORD(2, 2), &data);
    }
    void TearDown() override {
        WSACleanup();
    }
};
static ::testing::Environment *const winsock_environment = ::testing::AddGlobalTestEnvironment(new WinsockEnvironment);
#endif

static uint32_t loopback_ip() {
    uint32_t ip = 0;
    inet_pton(AF_INET, "127.0.0.1", &ip);
    return ip;
}

static void close_test_socket(abs_socket sock) {
#ifdef _WIN32
    closesocket(sock);
#else
    ::close(sock);
#endif
}

// Plain UDP socket bound to a free loopback port
static abs_socket open_udp_listener(uint16_t &port) {
    const abs_socket sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    inet_pton(AF_INET, "127.0.0.1", &addr.sin_addr);
    bind(sock, reinterpret_cast<const sockaddr *>(&addr), sizeof(addr));
    socklen_t len = sizeof(addr);
    getsockname(sock, reinterpret_cast<sockaddr *>(&addr), &len);
    port = ntohs(addr.sin_port);
    return sock;
}

static uint16_t free_udp_port() {
    uint16_t port;
    close_test_socket(open_udp_listener(port));
    return port;
}

static int expected(int error) {
    return error;
}

class P2PMuxTest : public ::testing::Test {
protected:
    P2PContextPtr ctx = std::make_shared<P2PContext>();
    uint16_t port_a = 0;
    uint16_t port_b = 0;

    void SetUp() override {
        ctx->bind_addr = loopback_ip();
        port_a = free_udp_port();
        port_b = free_udp_port();
    }

    void TearDown() override {
        ctx->shutdown();
    }

    std::shared_ptr<P2PMuxSocket> make_socket() {
        return std::make_shared<P2PMuxSocket>(ctx, AF_INET, SCE_NET_SOCK_DGRAM_P2P, 0);
    }

    static SceNetSockaddrIn p2p_addr(uint32_t ip, uint16_t port, uint16_t vport) {
        SceNetSockaddrIn addr{};
        addr.sin_len = 0; // PSASBR passes 0 here
        addr.sin_family = SCE_NET_AF_INET;
        addr.sin_port = htons(port);
        addr.sin_addr.s_addr = ip;
        addr.sin_vport = htons(vport);
        return addr;
    }

    std::shared_ptr<P2PMuxSocket> bound_socket(uint16_t port, uint16_t vport) {
        auto sock = make_socket();
        const auto addr = p2p_addr(0, port, vport);
        EXPECT_EQ(sock->bind(reinterpret_cast<const SceNetSockaddr *>(&addr), sizeof(addr)), 0);
        return sock;
    }

    static void set_int_option(Socket &sock, int optname, int value) {
        EXPECT_EQ(sock.set_socket_options(SCE_NET_SOL_SOCKET, optname, &value, sizeof(value)), 0);
    }

    static int send(Socket &sock, const std::string &payload, uint32_t ip, uint16_t port, uint16_t vport) {
        const auto to = p2p_addr(ip, port, vport);
        return sock.send_packet(payload.data(), static_cast<unsigned int>(payload.size()), 0, reinterpret_cast<const SceNetSockaddr *>(&to), sizeof(to));
    }
};

TEST(p2p_frame, layout_matches_console_format) {
    const char payload[] = { 0x00, 0x00, 0x00, 0x00, 0x08, 0x45, 0x71, 0x29 };
    const auto frame = p2p_make_frame(1001, 1002, payload, sizeof(payload));

    const std::vector<uint8_t> expected_frame = { 0xFF, 0x83, 0x03, 0xE9, 0x03, 0xEA, 0x00, 0x00, 0x00, 0x00, 0x08, 0x45, 0x71, 0x29 };
    EXPECT_EQ(frame, expected_frame);

    uint16_t src_vport = 0, dst_vport = 0;
    ASSERT_TRUE(p2p_parse_frame(frame.data(), frame.size(), src_vport, dst_vport));
    EXPECT_EQ(src_vport, 1001);
    EXPECT_EQ(dst_vport, 1002);
}

TEST(p2p_frame, rejects_foreign_datagrams) {
    uint16_t src_vport, dst_vport;
    const uint8_t short_frame[] = { 0xFF, 0x83, 0x03, 0xE8, 0x03 };
    EXPECT_FALSE(p2p_parse_frame(short_frame, sizeof(short_frame), src_vport, dst_vport));

    // Vita3K's legacy framing-less adhoc payload
    const uint8_t raw[] = { 0x01, 0x02, 0x03, 0xE8, 0x03, 0xE8, 0x00 };
    EXPECT_FALSE(p2p_parse_frame(raw, sizeof(raw), src_vport, dst_vport));

    // Header only, empty payload (PSASBR's 7-byte session boundary has one payload byte)
    const uint8_t header_only[] = { 0xFF, 0x83, 0x03, 0xE8, 0x03, 0xE8 };
    EXPECT_TRUE(p2p_parse_frame(header_only, sizeof(header_only), src_vport, dst_vport));
}

TEST(p2p_config, parses_forward_list) {
    const auto endpoints = P2PContext::parse_forward_list(" 127.0.0.1:4000 ,10.0.0.2:5000,bad,1.2.3.4:0,:80,, 5.6.7.8:");
    ASSERT_EQ(endpoints.size(), 2u);
    EXPECT_EQ(ntohs(endpoints[0].sin_port), 4000);
    EXPECT_EQ(ntohs(endpoints[1].sin_port), 5000);
    uint32_t ip;
    std::memcpy(&ip, &endpoints[0].sin_addr, sizeof(ip));
    EXPECT_EQ(ip, loopback_ip());
}

TEST(p2p_config, parses_bind_address) {
    EXPECT_EQ(P2PContext::parse_bind_address(""), 0u);
    EXPECT_EQ(P2PContext::parse_bind_address("  "), 0u);
    EXPECT_EQ(P2PContext::parse_bind_address("127.0.0.1"), loopback_ip());
    EXPECT_EQ(P2PContext::parse_bind_address("not an ip"), 0u);
}

TEST_F(P2PMuxTest, dispatches_by_destination_vport) {
    const auto sender = bound_socket(port_a, 1001);
    const auto browser = bound_socket(port_b, 1002);
    const auto other = bound_socket(port_b, 1000);
    set_int_option(*browser, SCE_NET_SO_RCVTIMEO, 1000000);
    set_int_option(*other, SCE_NET_SO_NBIO, 1);

    ASSERT_EQ(send(*sender, "search", loopback_ip(), port_b, 1002), 6);

    char buf[64] = {};
    SceNetSockaddrIn from{};
    unsigned int fromlen = sizeof(from);
    ASSERT_EQ(browser->recv_packet(buf, sizeof(buf), 0, reinterpret_cast<SceNetSockaddr *>(&from), &fromlen), 6);
    EXPECT_EQ(std::string(buf, 6), "search");
    EXPECT_EQ(fromlen, sizeof(SceNetSockaddrIn));
    EXPECT_EQ(from.sin_len, sizeof(SceNetSockaddrIn));
    EXPECT_EQ(from.sin_family, SCE_NET_AF_INET);
    EXPECT_EQ(from.sin_addr.s_addr, loopback_ip());
    EXPECT_EQ(ntohs(from.sin_port), port_a);
    EXPECT_EQ(ntohs(from.sin_vport), 1001);

    EXPECT_EQ(other->recv_packet(buf, sizeof(buf), 0, nullptr, nullptr), expected(SCE_NET_ERROR_EWOULDBLOCK));
}

TEST_F(P2PMuxTest, drops_frames_for_unbound_vports) {
    const auto sender = bound_socket(port_a, 1000);
    const auto receiver = bound_socket(port_b, 1000);
    set_int_option(*receiver, SCE_NET_SO_RCVTIMEO, 1000000);

    ASSERT_EQ(send(*sender, "stale", loopback_ip(), port_b, 4242), 5);
    ASSERT_EQ(send(*sender, "live", loopback_ip(), port_b, 1000), 4);

    char buf[16] = {};
    ASSERT_EQ(receiver->recv_packet(buf, sizeof(buf), 0, nullptr, nullptr), 4);
    EXPECT_EQ(std::string(buf, 4), "live");
}

TEST_F(P2PMuxTest, nonblocking_and_timeout_report_would_block) {
    const auto sock = bound_socket(port_a, 1000);
    char buf[8];

    EXPECT_EQ(sock->recv_packet(buf, sizeof(buf), SCE_NET_MSG_DONTWAIT, nullptr, nullptr), expected(SCE_NET_ERROR_EWOULDBLOCK));

    set_int_option(*sock, SCE_NET_SO_RCVTIMEO, 20000);
    const auto start = std::chrono::steady_clock::now();
    EXPECT_EQ(sock->recv_packet(buf, sizeof(buf), 0, nullptr, nullptr), expected(SCE_NET_ERROR_EWOULDBLOCK));
    EXPECT_GE(std::chrono::steady_clock::now() - start, std::chrono::milliseconds(15));
}

TEST_F(P2PMuxTest, abort_wakes_blocked_receive) {
    const auto sock = bound_socket(port_a, 1000);

    auto pending = std::async(std::launch::async, [&] {
        char buf[8];
        return sock->recv_packet(buf, sizeof(buf), 0, nullptr, nullptr);
    });
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
    EXPECT_EQ(sock->abort(0), 0);
    ASSERT_EQ(pending.wait_for(std::chrono::seconds(2)), std::future_status::ready);
    EXPECT_EQ(pending.get(), expected(SCE_NET_ERROR_EINTR));

    // Without a preservation flag the socket keeps working after the abort
    char buf[8];
    EXPECT_EQ(sock->recv_packet(buf, sizeof(buf), SCE_NET_MSG_DONTWAIT, nullptr, nullptr), expected(SCE_NET_ERROR_EWOULDBLOCK));

    sock->abort(SCE_NET_SOCKET_ABORT_FLAG_RCV_PRESERVATION);
    EXPECT_EQ(sock->recv_packet(buf, sizeof(buf), SCE_NET_MSG_DONTWAIT, nullptr, nullptr), expected(SCE_NET_ERROR_EINTR));
}

TEST_F(P2PMuxTest, peek_keeps_the_datagram) {
    const auto sender = bound_socket(port_a, 1000);
    const auto receiver = bound_socket(port_b, 1000);
    set_int_option(*receiver, SCE_NET_SO_RCVTIMEO, 1000000);
    ASSERT_EQ(send(*sender, "hello", loopback_ip(), port_b, 1000), 5);

    char buf[3] = {};
    EXPECT_EQ(receiver->recv_packet(buf, sizeof(buf), SCE_NET_MSG_PEEK, nullptr, nullptr), 3);
    EXPECT_EQ(receiver->recv_packet(nullptr, 0, SCE_NET_MSG_PEEKLEN, nullptr, nullptr), 5);
    char full[8] = {};
    EXPECT_EQ(receiver->recv_packet(full, sizeof(full), 0, nullptr, nullptr), 5);
    EXPECT_EQ(std::string(full, 5), "hello");
    EXPECT_EQ(receiver->recv_packet(full, sizeof(full), SCE_NET_MSG_DONTWAIT, nullptr, nullptr), expected(SCE_NET_ERROR_EWOULDBLOCK));
}

TEST_F(P2PMuxTest, vport_sharing_requires_reuse) {
    const auto first = bound_socket(port_a, 1001);
    auto second = make_socket();
    const auto addr = p2p_addr(0, port_a, 1001);
    EXPECT_EQ(second->bind(reinterpret_cast<const SceNetSockaddr *>(&addr), sizeof(addr)), expected(SCE_NET_ERROR_EADDRINUSE));

    set_int_option(*first, SCE_NET_SO_REUSEADDR, 1);
    EXPECT_EQ(second->bind(reinterpret_cast<const SceNetSockaddr *>(&addr), sizeof(addr)), 0);

    // Closing frees the vport again
    first->close();
    second->close();
    auto third = make_socket();
    EXPECT_EQ(third->bind(reinterpret_cast<const SceNetSockaddr *>(&addr), sizeof(addr)), 0);
}

TEST_F(P2PMuxTest, reports_bound_address_and_auto_vport) {
    const auto sock = bound_socket(port_a, 0);
    SceNetSockaddrIn name{};
    unsigned int namelen = 0;
    ASSERT_EQ(sock->get_socket_address(reinterpret_cast<SceNetSockaddr *>(&name), &namelen), 0);
    EXPECT_EQ(namelen, sizeof(SceNetSockaddrIn));
    EXPECT_EQ(ntohs(name.sin_port), port_a);
    EXPECT_EQ(ntohs(name.sin_vport), P2P_FIRST_AUTO_VPORT);
    EXPECT_EQ(name.sin_addr.s_addr, loopback_ip());

    int type = 0;
    unsigned int typelen = sizeof(type);
    ASSERT_EQ(sock->get_socket_options(SCE_NET_SOL_SOCKET, SCE_NET_SO_TYPE, &type, &typelen), 0);
    EXPECT_EQ(type, SCE_NET_SOCK_DGRAM_P2P);
}

TEST_F(P2PMuxTest, broadcast_is_forwarded_to_configured_endpoints) {
    uint16_t forward_port;
    const abs_socket listener = open_udp_listener(forward_port);
    sockaddr_in endpoint{};
    endpoint.sin_family = AF_INET;
    endpoint.sin_port = htons(forward_port);
    inet_pton(AF_INET, "127.0.0.1", &endpoint.sin_addr);
    ctx->broadcast_forward = { endpoint };

    const auto sender = bound_socket(port_a, 1001);
    // Succeeds even where a broadcast from a loopback-bound socket is refused by the OS
    ASSERT_EQ(send(*sender, "advert", INADDR_BROADCAST, port_b, 1002), 6);

    fd_set fds;
    FD_ZERO(&fds);
    FD_SET(listener, &fds);
    timeval timeout{ 2, 0 };
    ASSERT_EQ(select(static_cast<int>(listener) + 1, &fds, nullptr, nullptr, &timeout), 1);

    uint8_t buf[64];
    sockaddr_in from{};
    socklen_t fromlen = sizeof(from);
    const auto res = recvfrom(listener, reinterpret_cast<char *>(buf), sizeof(buf), 0, reinterpret_cast<sockaddr *>(&from), &fromlen);
    ASSERT_EQ(res, static_cast<int>(P2P_FRAME_HEADER_SIZE + 6));
    const std::vector<uint8_t> header(buf, buf + P2P_FRAME_HEADER_SIZE);
    const std::vector<uint8_t> expected_header = { 0xFF, 0x83, 0x03, 0xE9, 0x03, 0xEA };
    EXPECT_EQ(header, expected_header);
    // The copy comes from the shared P2P socket, like the real broadcast
    EXPECT_EQ(ntohs(from.sin_port), port_a);

    close_test_socket(listener);
}

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

#include <net/socket.h>

#include <atomic>
#include <condition_variable>
#include <deque>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

// Console-compatible SCE_NET_SOCK_DGRAM_P2P transport.
// On real hardware every P2P vport of a port shares a single UDP socket, and each datagram
// starts with [0xFF 0x83][src_vport BE16][dst_vport BE16] followed by the payload; the kernel
// demultiplexes by the destination vport. Speaking this format lets games talk to real
// PS Vita / PS3 consoles and to RPCS3, instead of only to other Vita3K instances.

constexpr uint8_t P2P_FRAME_MAGIC_0 = 0xFF;
constexpr uint8_t P2P_FRAME_MAGIC_1 = 0x83;
constexpr size_t P2P_FRAME_HEADER_SIZE = 6;
constexpr uint16_t P2P_FIRST_AUTO_VPORT = 30000;
constexpr size_t P2P_MAX_QUEUED_DATAGRAMS = 256;

struct P2PMuxSocket;

struct P2PDatagram {
    uint32_t addr; // Source IPv4 address, network order
    uint16_t port; // Source native UDP port, host order
    uint16_t vport; // Source vport, host order
    std::vector<uint8_t> data;
};

std::vector<uint8_t> p2p_make_frame(uint16_t src_vport, uint16_t dst_vport, const void *data, size_t len);
// Returns false if the datagram is not a console P2P frame
bool p2p_parse_frame(const uint8_t *data, size_t len, uint16_t &src_vport, uint16_t &dst_vport);

// One native UDP socket shared by every vport bound on a port, with a thread dispatching received frames
struct P2PPort {
    const uint16_t port;

    P2PPort(uint16_t port, uint32_t bind_addr);
    ~P2PPort();
    P2PPort(const P2PPort &) = delete;
    P2PPort &operator=(const P2PPort &) = delete;

    // Returns 0 or an SCE_NET_ERROR_* code. vport 0 picks a free vport.
    int bind_vport(const std::shared_ptr<P2PMuxSocket> &sock, uint16_t &vport);
    void unbind_vport(const P2PMuxSocket *sock, uint16_t vport);
    // Returns the number of bytes sent or an SCE_NET_ERROR_* code
    int send_to(const std::vector<uint8_t> &frame, const sockaddr_in &to);

    int open_error() const { return error; }
    uint32_t bound_addr() const { return bind_addr; }
    void close();

private:
    struct Binding {
        const P2PMuxSocket *raw;
        std::weak_ptr<P2PMuxSocket> sock;
    };

    abs_socket sock;
    uint32_t bind_addr = 0; // network order
    int error = 0;
    std::atomic<bool> running = false;
    std::thread recv_thread;
    std::mutex vports_mutex;
    std::map<uint16_t, std::vector<Binding>> vports;

    void recv_loop();
    void dispatch(const sockaddr_in &from, const uint8_t *data, size_t len);
};

typedef std::shared_ptr<P2PPort> P2PPortPtr;

// Per-emulation console P2P state: settings and the ports opened on demand
struct P2PContext {
    // IPv4 the native sockets bind to, network order (0 = INADDR_ANY)
    uint32_t bind_addr = 0;
    // Unicast copies of every broadcast frame go to these endpoints, e.g. a LAN connector on the same PC
    std::vector<sockaddr_in> broadcast_forward;
    // Subnet broadcast address, also treated as a broadcast destination (network order)
    std::atomic<uint32_t> broadcast_addr = 0xFFFFFFFF;

    // Returns the port, opening it if needed, or nullptr with err set
    P2PPortPtr get_port(uint16_t port, int &err);
    // Address (network order) the native socket of a port is bound to; false while that port is not open
    bool get_bound_addr(uint16_t port, uint32_t &addr);
    // Closes every port (and its native socket)
    void shutdown();

    static uint32_t parse_bind_address(const std::string &str);
    static std::vector<sockaddr_in> parse_forward_list(const std::string &str);

private:
    std::mutex mutex;
    std::map<uint16_t, P2PPortPtr> ports;
};

typedef std::shared_ptr<P2PContext> P2PContextPtr;

struct P2PMuxSocket : public Socket, public std::enable_shared_from_this<P2PMuxSocket> {
    explicit P2PMuxSocket(P2PContextPtr ctx, int domain, int type, int protocol);
    ~P2PMuxSocket() override;

    int abort(int flags) override;
    int close() override;
    int shutdown_socket(int how) override;
    int bind(const SceNetSockaddr *addr, unsigned int addrlen) override;
    int send_packet(const void *msg, unsigned int len, int flags, const SceNetSockaddr *to, unsigned int tolen) override;
    int recv_packet(void *buf, unsigned int len, int flags, SceNetSockaddr *from, unsigned int *fromlen) override;
    int set_socket_options(int level, int optname, const void *optval, unsigned int optlen) override;
    int get_socket_options(int level, int optname, void *optval, unsigned int *optlen) override;
    int connect(const SceNetSockaddr *addr, unsigned int addrlen) override;
    SocketPtr accept(SceNetSockaddr *addr, unsigned int *addrlen, int &err) override;
    int listen(int backlog) override;
    int get_peer_address(SceNetSockaddr *addr, unsigned int *addrlen) override;
    int get_socket_address(SceNetSockaddr *addr, unsigned int *addrlen) override;

    // SCE_NET_EPOLL* readiness, since this socket has no native handle to select() on
    unsigned int poll_events();
    bool is_reusable();
    void push_datagram(P2PDatagram &&dgram);

private:
    P2PContextPtr ctx;
    P2PPortPtr port;
    uint16_t vport = 0;
    bool closed = false;

    std::mutex mutex;
    std::condition_variable cond;
    std::deque<P2PDatagram> queue;
    int abort_flags = 0;
    uint32_t abort_generation = 0;

    int sockopt_so_nbio = 0;
    int sockopt_so_reuseaddr = 0;
    int sockopt_so_reuseport = 0;
    int sockopt_so_rcvtimeo = 0; // microseconds, 0 = infinite
    std::map<std::pair<int, int>, int> sockopts;

    void unbind();
};

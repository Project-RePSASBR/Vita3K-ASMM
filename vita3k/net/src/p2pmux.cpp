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

#include <util/log.h>

#include <algorithm>
#include <charconv>
#include <chrono>
#include <cstring>

#ifndef _WIN32
#include <fcntl.h>
#endif

#ifdef _WIN32
#ifndef SIO_UDP_CONNRESET
#define SIO_UDP_CONNRESET _WSAIOW(IOC_VENDOR, 12)
#endif
static constexpr abs_socket INVALID_NATIVE_SOCKET = INVALID_SOCKET;
#else
static constexpr abs_socket INVALID_NATIVE_SOCKET = -1;
#endif

// s_addr is not usable on native in_addr on Windows (socket.h undefines the macro), so copy the raw bytes
static uint32_t get_native_ip(const sockaddr_in &addr) {
    uint32_t ip;
    std::memcpy(&ip, &addr.sin_addr, sizeof(ip));
    return ip;
}

static void set_native_ip(sockaddr_in &addr, uint32_t ip) {
    std::memcpy(&addr.sin_addr, &ip, sizeof(ip));
}

static std::string ip_to_string(uint32_t ip) {
    char str[INET_ADDRSTRLEN] = {};
    inet_ntop(AF_INET, &ip, str, sizeof(str));
    return str;
}

static void close_native_socket(abs_socket sock) {
#ifdef _WIN32
    closesocket(sock);
#else
    ::close(sock);
#endif
}

static std::string trim(const std::string &str) {
    const auto begin = str.find_first_not_of(" \t");
    if (begin == std::string::npos)
        return {};
    const auto end = str.find_last_not_of(" \t");
    return str.substr(begin, end - begin + 1);
}

std::vector<uint8_t> p2p_make_frame(uint16_t src_vport, uint16_t dst_vport, const void *data, size_t len) {
    std::vector<uint8_t> frame(P2P_FRAME_HEADER_SIZE + len);
    frame[0] = P2P_FRAME_MAGIC_0;
    frame[1] = P2P_FRAME_MAGIC_1;
    frame[2] = static_cast<uint8_t>(src_vport >> 8);
    frame[3] = static_cast<uint8_t>(src_vport & 0xFF);
    frame[4] = static_cast<uint8_t>(dst_vport >> 8);
    frame[5] = static_cast<uint8_t>(dst_vport & 0xFF);
    if (len)
        std::memcpy(frame.data() + P2P_FRAME_HEADER_SIZE, data, len);
    return frame;
}

bool p2p_parse_frame(const uint8_t *data, size_t len, uint16_t &src_vport, uint16_t &dst_vport) {
    if (!data || len < P2P_FRAME_HEADER_SIZE || data[0] != P2P_FRAME_MAGIC_0 || data[1] != P2P_FRAME_MAGIC_1)
        return false;

    src_vport = static_cast<uint16_t>((data[2] << 8) | data[3]);
    dst_vport = static_cast<uint16_t>((data[4] << 8) | data[5]);
    return true;
}

P2PPort::P2PPort(uint16_t port, uint32_t requested_addr)
    : port(port)
    , sock(::socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP)) {
    if (sock == INVALID_NATIVE_SOCKET) {
        error = PosixSocket::translate_return_value(-1);
        LOG_ERROR("Console P2P: failed to create the socket for port {}: {}", port, log_hex(error));
        return;
    }

    const int one = 1;
    if (setsockopt(sock, SOL_SOCKET, SO_BROADCAST, reinterpret_cast<const char *>(&one), sizeof(one)) != 0)
        LOG_ERROR("Console P2P: failed to enable SO_BROADCAST on port {}", port);
#ifdef _WIN32
    // Windows queues an error on a UDP socket when one of our earlier sends bounced with ICMP
    // port-unreachable, and the next recvfrom then fails with WSAECONNRESET. Consoles never see
    // errors on UDP, so opt out or a peer leaving the lobby starves every other peer's traffic.
    BOOL report_connreset = FALSE;
    DWORD bytes_returned = 0;
    if (WSAIoctl(sock, SIO_UDP_CONNRESET, &report_connreset, sizeof(report_connreset), nullptr, 0, &bytes_returned, nullptr, nullptr) != 0)
        LOG_ERROR("Console P2P: failed to disable SIO_UDP_CONNRESET on port {}", port);
    u_long nonblocking = 1;
    ioctlsocket(sock, FIONBIO, &nonblocking);
#else
    // Lets a specific bind address coexist with a wildcard socket on the same port (e.g. a LAN connector)
    setsockopt(sock, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
    fcntl(sock, F_SETFL, fcntl(sock, F_GETFL, 0) | O_NONBLOCK);
#endif
    const int rcvbuf = 131072;
    setsockopt(sock, SOL_SOCKET, SO_RCVBUF, reinterpret_cast<const char *>(&rcvbuf), sizeof(rcvbuf));

    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(port);
    set_native_ip(addr, requested_addr);
    int res = ::bind(sock, reinterpret_cast<const sockaddr *>(&addr), sizeof(addr));
    if ((res < 0) && (requested_addr != 0)) {
        LOG_ERROR("Console P2P: failed to bind to {}:{}, falling back to 0.0.0.0:{}", ip_to_string(requested_addr), port, port);
        set_native_ip(addr, 0);
        res = ::bind(sock, reinterpret_cast<const sockaddr *>(&addr), sizeof(addr));
    }
    if (res < 0) {
        error = PosixSocket::translate_return_value(res);
        LOG_ERROR("Console P2P: failed to bind UDP port {} ({}), is another emulator or connector using it?", port, log_hex(error));
        close_native_socket(sock);
        sock = INVALID_NATIVE_SOCKET;
        return;
    }

    bind_addr = get_native_ip(addr);
    running = true;
    recv_thread = std::thread(&P2PPort::recv_loop, this);
    LOG_INFO("Console P2P: port {} bound to {}", port, ip_to_string(bind_addr));
}

P2PPort::~P2PPort() {
    close();
}

void P2PPort::close() {
    running = false;
    if (recv_thread.joinable()) {
        // The last reference can be dropped by the receive thread itself
        if (recv_thread.get_id() == std::this_thread::get_id())
            recv_thread.detach();
        else
            recv_thread.join();
    }

    if (sock != INVALID_NATIVE_SOCKET) {
        close_native_socket(sock);
        sock = INVALID_NATIVE_SOCKET;
    }
}

void P2PPort::recv_loop() {
    std::vector<uint8_t> buf(65536);
    while (running) {
        fd_set fds;
        FD_ZERO(&fds);
        FD_SET(sock, &fds);
        // The timeout bounds how long close() waits for this thread
        timeval timeout{ 0, 50000 };
        const int ready = select(static_cast<int>(sock) + 1, &fds, nullptr, nullptr, &timeout);
        if (ready < 0) {
            LOG_ERROR_ONCE("Console P2P: select failed on port {}", port);
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
            continue;
        }
        if (ready == 0)
            continue;

        // Drain everything that is queued before waiting again
        while (running) {
            sockaddr_in from{};
            socklen_t fromlen = sizeof(from);
            const auto res = recvfrom(sock, reinterpret_cast<char *>(buf.data()), static_cast<int>(buf.size()), 0, reinterpret_cast<sockaddr *>(&from), &fromlen);
            if (res < 0) {
#ifdef _WIN32
                // An earlier send of ours bounced; the receive stream itself is fine
                if (WSAGetLastError() == WSAECONNRESET)
                    continue;
#endif
                break;
            }

            dispatch(from, buf.data(), static_cast<size_t>(res));
        }
    }
}

void P2PPort::dispatch(const sockaddr_in &from, const uint8_t *data, size_t len) {
    uint16_t src_vport, dst_vport;
    if (!p2p_parse_frame(data, len, src_vport, dst_vport)) {
        LOG_WARN_ONCE("Console P2P: dropping a datagram without P2P framing from {}:{}", ip_to_string(get_native_ip(from)), ntohs(from.sin_port));
        return;
    }

    // Collect the targets first: pushing (or dropping the last reference to a socket) must not happen under the lock
    std::vector<std::shared_ptr<P2PMuxSocket>> targets;
    {
        const std::lock_guard<std::mutex> lock(vports_mutex);
        const auto it = vports.find(dst_vport);
        if (it == vports.end()) {
            // Stale traffic for a vport nobody listens on (anymore)
            LOG_TRACE("Console P2P: dropping a datagram for unbound vport {}", dst_vport);
            return;
        }

        for (const auto &binding : it->second) {
            if (auto target = binding.sock.lock())
                targets.push_back(std::move(target));
        }
    }

    for (const auto &target : targets) {
        target->push_datagram(P2PDatagram{
            get_native_ip(from),
            ntohs(from.sin_port),
            src_vport,
            std::vector<uint8_t>(data + P2P_FRAME_HEADER_SIZE, data + len),
        });
    }
}

int P2PPort::bind_vport(const std::shared_ptr<P2PMuxSocket> &new_sock, uint16_t &vport) {
    const std::lock_guard<std::mutex> lock(vports_mutex);

    if (vport == 0) {
        vport = P2P_FIRST_AUTO_VPORT;
        while (vports.contains(vport))
            ++vport;
    }

    auto &bindings = vports[vport];
    std::erase_if(bindings, [](const Binding &binding) { return binding.sock.expired(); });

    // Sharing a vport requires every socket already bound to it to allow address reuse
    for (const auto &binding : bindings) {
        const auto bound_sock = binding.sock.lock();
        if (bound_sock && !bound_sock->is_reusable())
            return SCE_NET_ERROR_EADDRINUSE;
    }

    bindings.push_back(Binding{ new_sock.get(), new_sock });
    return 0;
}

void P2PPort::unbind_vport(const P2PMuxSocket *raw_sock, uint16_t vport) {
    const std::lock_guard<std::mutex> lock(vports_mutex);
    const auto it = vports.find(vport);
    if (it == vports.end())
        return;

    std::erase_if(it->second, [raw_sock](const Binding &binding) {
        return (binding.raw == raw_sock) || binding.sock.expired();
    });
    if (it->second.empty())
        vports.erase(it);
}

int P2PPort::send_to(const std::vector<uint8_t> &frame, const sockaddr_in &to) {
    if (sock == INVALID_NATIVE_SOCKET)
        return SCE_NET_ERROR_EBADF;

    const auto res = sendto(sock, reinterpret_cast<const char *>(frame.data()), static_cast<int>(frame.size()), 0, reinterpret_cast<const sockaddr *>(&to), sizeof(to));
    return PosixSocket::translate_return_value(static_cast<int>(res));
}

P2PPortPtr P2PContext::get_port(uint16_t port, int &err) {
    const std::lock_guard<std::mutex> lock(mutex);
    const auto it = ports.find(port);
    if (it != ports.end())
        return it->second;

    auto new_port = std::make_shared<P2PPort>(port, bind_addr);
    if (new_port->open_error()) {
        err = new_port->open_error();
        return nullptr;
    }

    ports.emplace(port, new_port);
    return new_port;
}

bool P2PContext::get_bound_addr(uint16_t port, uint32_t &addr) {
    const std::lock_guard<std::mutex> lock(mutex);
    const auto it = ports.find(port);
    if (it == ports.end())
        return false;

    addr = it->second->bound_addr();
    return true;
}

void P2PContext::shutdown() {
    std::map<uint16_t, P2PPortPtr> closing;
    {
        const std::lock_guard<std::mutex> lock(mutex);
        closing.swap(ports);
    }

    // Sockets may still hold a port, so close it explicitly instead of relying on the last reference
    for (auto &[number, port] : closing)
        port->close();
}

uint32_t P2PContext::parse_bind_address(const std::string &str) {
    const auto addr_str = trim(str);
    if (addr_str.empty())
        return 0;

    uint32_t addr = 0;
    if (inet_pton(AF_INET, addr_str.c_str(), &addr) != 1) {
        LOG_ERROR("Console P2P: invalid bind address '{}', using 0.0.0.0", addr_str);
        return 0;
    }

    return addr;
}

std::vector<sockaddr_in> P2PContext::parse_forward_list(const std::string &str) {
    std::vector<sockaddr_in> endpoints;
    size_t start = 0;
    while (start <= str.size()) {
        auto end = str.find(',', start);
        if (end == std::string::npos)
            end = str.size();
        const auto entry = trim(str.substr(start, end - start));
        start = end + 1;
        if (entry.empty())
            continue;

        const auto sep = entry.rfind(':');
        uint16_t port = 0;
        uint32_t addr = 0;
        const bool valid = (sep != std::string::npos) && (sep != 0) && (sep + 1 < entry.size())
            && (std::from_chars(entry.data() + sep + 1, entry.data() + entry.size(), port).ec == std::errc())
            && (port != 0) && (inet_pton(AF_INET, entry.substr(0, sep).c_str(), &addr) == 1);
        if (!valid) {
            LOG_ERROR("Console P2P: invalid broadcast forward entry '{}' (expected ip:port)", entry);
            continue;
        }

        sockaddr_in endpoint{};
        endpoint.sin_family = AF_INET;
        endpoint.sin_port = htons(port);
        set_native_ip(endpoint, addr);
        endpoints.push_back(endpoint);
    }

    return endpoints;
}

P2PMuxSocket::P2PMuxSocket(P2PContextPtr ctx, int domain, int type, int protocol)
    : Socket(domain, type, protocol)
    , ctx(std::move(ctx)) {}

P2PMuxSocket::~P2PMuxSocket() {
    unbind();
}

void P2PMuxSocket::unbind() {
    P2PPortPtr bound_port;
    uint16_t bound_vport;
    {
        const std::lock_guard<std::mutex> lock(mutex);
        bound_port = std::move(port);
        bound_vport = vport;
    }

    if (bound_port)
        bound_port->unbind_vport(this, bound_vport);
}

int P2PMuxSocket::abort(int flags) {
    {
        const std::lock_guard<std::mutex> lock(mutex);
        abort_flags |= flags;
        ++abort_generation;
    }
    cond.notify_all();
    return 0;
}

int P2PMuxSocket::close() {
    {
        const std::lock_guard<std::mutex> lock(mutex);
        closed = true;
        queue.clear();
    }
    cond.notify_all();
    unbind();
    return 0;
}

int P2PMuxSocket::shutdown_socket(int how) {
    return 0;
}

int P2PMuxSocket::bind(const SceNetSockaddr *addr, unsigned int addrlen) {
    if (!addr)
        return SCE_NET_ERROR_EINVAL;

    {
        const std::lock_guard<std::mutex> lock(mutex);
        if (closed)
            return SCE_NET_ERROR_EBADF;
        if (port)
            return SCE_NET_ERROR_EINVAL;
    }

    // Games may pass sin_len = 0, so only the port and vport are looked at. Port 0 is the default P2P port.
    const auto *addr_in = reinterpret_cast<const SceNetSockaddrIn *>(addr);
    uint16_t native_port = ntohs(addr_in->sin_port);
    if (native_port == 0)
        native_port = SCE_NET_ADHOC_PORT;

    int err = 0;
    auto new_port = ctx->get_port(native_port, err);
    if (!new_port)
        return err;

    uint16_t new_vport = ntohs(addr_in->sin_vport);
    if (const int res = new_port->bind_vport(shared_from_this(), new_vport); res < 0)
        return res;

    const std::lock_guard<std::mutex> lock(mutex);
    port = std::move(new_port);
    vport = new_vport;
    return 0;
}

int P2PMuxSocket::send_packet(const void *msg, unsigned int len, int flags, const SceNetSockaddr *to, unsigned int tolen) {
    if (!to)
        return SCE_NET_ERROR_EDESTADDRREQ;
    if (len > 65507 - P2P_FRAME_HEADER_SIZE)
        return SCE_NET_ERROR_EMSGSIZE;

    bool is_bound;
    {
        const std::lock_guard<std::mutex> lock(mutex);
        if (closed)
            return SCE_NET_ERROR_EBADF;
        if (abort_flags & SCE_NET_SOCKET_ABORT_FLAG_SND_PRESERVATION)
            return SCE_NET_ERROR_EINTR;
        is_bound = port != nullptr;
    }

    // Sending from an unbound socket implicitly binds it to the default port with a free vport
    if (!is_bound) {
        SceNetSockaddrIn any{};
        any.sin_len = sizeof(any);
        any.sin_family = SCE_NET_AF_INET;
        any.sin_port = htons(SCE_NET_ADHOC_PORT);
        if (const int res = bind(reinterpret_cast<const SceNetSockaddr *>(&any), sizeof(any)); res < 0)
            return res;
    }

    P2PPortPtr send_port;
    uint16_t src_vport;
    {
        const std::lock_guard<std::mutex> lock(mutex);
        send_port = port;
        src_vport = vport;
    }
    if (!send_port)
        return SCE_NET_ERROR_EBADF;

    const auto *to_in = reinterpret_cast<const SceNetSockaddrIn *>(to);
    sockaddr_in dest{};
    dest.sin_family = AF_INET;
    dest.sin_port = to_in->sin_port ? to_in->sin_port : htons(SCE_NET_ADHOC_PORT);
    set_native_ip(dest, to_in->sin_addr.s_addr);

    const auto frame = p2p_make_frame(src_vport, ntohs(to_in->sin_vport), msg, len);

    const uint32_t dest_ip = get_native_ip(dest);
    const bool is_broadcast = (dest_ip == INADDR_BROADCAST) || (dest_ip == ctx->broadcast_addr.load());
    if (is_broadcast && !ctx->broadcast_forward.empty()) {
        // Forward unicast copies first, so an external relay/gateway that doesn't share a broadcast
        // domain with the emulator still receives lobby beacons
        for (const auto &endpoint : ctx->broadcast_forward)
            send_port->send_to(frame, endpoint);
        LOG_INFO_ONCE("Console P2P: broadcast forward active, first copy sent to {}:{}", ip_to_string(get_native_ip(ctx->broadcast_forward.front())), ntohs(ctx->broadcast_forward.front().sin_port));
    }

    const int res = send_port->send_to(frame, dest);
    if (res < 0) {
        // A broadcast from a loopback-bound socket can fail; the forwarded copies are what matter then
        if (is_broadcast && !ctx->broadcast_forward.empty()) {
            LOG_WARN_ONCE("Console P2P: broadcast send failed ({}), relying on broadcast forward", log_hex(res));
            return static_cast<int>(len);
        }
        return res;
    }

    return static_cast<int>(len);
}

int P2PMuxSocket::recv_packet(void *buf, unsigned int len, int flags, SceNetSockaddr *from, unsigned int *fromlen) {
    std::unique_lock<std::mutex> lock(mutex);
    if (closed)
        return SCE_NET_ERROR_EBADF;
    if (abort_flags & SCE_NET_SOCKET_ABORT_FLAG_RCV_PRESERVATION)
        return SCE_NET_ERROR_EINTR;

    if (queue.empty()) {
        if (sockopt_so_nbio || (flags & SCE_NET_MSG_DONTWAIT))
            return SCE_NET_ERROR_EWOULDBLOCK;

        const auto generation = abort_generation;
        const auto ready = [&] { return !queue.empty() || closed || (abort_generation != generation); };
        if (sockopt_so_rcvtimeo > 0) {
            if (!cond.wait_for(lock, std::chrono::microseconds(sockopt_so_rcvtimeo), ready))
                return SCE_NET_ERROR_EWOULDBLOCK;
        } else {
            cond.wait(lock, ready);
        }

        if (closed || (abort_generation != generation))
            return SCE_NET_ERROR_EINTR;
    }

    const auto &dgram = queue.front();
    const auto copied = std::min<size_t>(len, dgram.data.size());
    if (buf && copied)
        std::memcpy(buf, dgram.data.data(), copied);

    if (from) {
        SceNetSockaddrIn from_in{};
        from_in.sin_len = sizeof(from_in);
        from_in.sin_family = SCE_NET_AF_INET;
        from_in.sin_port = htons(dgram.port);
        from_in.sin_addr.s_addr = dgram.addr;
        from_in.sin_vport = htons(dgram.vport);
        std::memcpy(from, &from_in, sizeof(from_in));
        if (fromlen)
            *fromlen = sizeof(from_in);
    }

    // PEEKLEN reports the full size of the next datagram
    const int result = static_cast<int>(((flags & SCE_NET_MSG_PEEKLEN) == SCE_NET_MSG_PEEKLEN) ? dgram.data.size() : copied);
    if (!(flags & SCE_NET_MSG_PEEK))
        queue.pop_front();

    return result;
}

int P2PMuxSocket::set_socket_options(int level, int optname, const void *optval, unsigned int optlen) {
    if ((level == SCE_NET_SOL_SOCKET) && (optname == SCE_NET_SO_NAME))
        return SCE_NET_ERROR_EINVAL; // don't support set for name
    if (!optval || (optlen != sizeof(int)))
        return SCE_NET_ERROR_EFAULT;

    int value;
    std::memcpy(&value, optval, sizeof(value));

    const std::lock_guard<std::mutex> lock(mutex);
    if (level == SCE_NET_SOL_SOCKET) {
        switch (optname) {
        case SCE_NET_SO_NBIO: sockopt_so_nbio = value; break;
        case SCE_NET_SO_REUSEADDR: sockopt_so_reuseaddr = value; break;
        case SCE_NET_SO_REUSEPORT: sockopt_so_reuseport = value; break;
        case SCE_NET_SO_ONESBCAST: sockopt_so_onesbcast = value; break;
        case SCE_NET_SO_RCVTIMEO: sockopt_so_rcvtimeo = std::max(value, 0); break;
        default: break;
        }
    }

    // Everything else (broadcast, buffer sizes, crypto...) has no effect on the shared native socket
    sockopts[{ level, optname }] = value;
    return 0;
}

int P2PMuxSocket::get_socket_options(int level, int optname, void *optval, unsigned int *optlen) {
    if (!optval || !optlen)
        return SCE_NET_ERROR_EFAULT;

    if ((level == SCE_NET_SOL_SOCKET) && (optname == SCE_NET_SO_NAME)) {
        if (*optlen < 1)
            return SCE_NET_ERROR_EFAULT;
        *static_cast<char *>(optval) = '\0'; // writes an empty string to the output buffer
        *optlen = 1;
        return 0;
    }

    if (*optlen < sizeof(int)) {
        *optlen = sizeof(int);
        return SCE_NET_ERROR_EFAULT;
    }

    int value = 0;
    {
        const std::lock_guard<std::mutex> lock(mutex);
        if ((level == SCE_NET_SOL_SOCKET) && (optname == SCE_NET_SO_TYPE))
            value = sce_type;
        else if ((level == SCE_NET_SOL_SOCKET) && (optname == SCE_NET_SO_ERROR))
            value = 0;
        else if (const auto it = sockopts.find({ level, optname }); it != sockopts.end())
            value = it->second;
        else if ((level == SCE_NET_SOL_SOCKET) && ((optname == SCE_NET_SO_RCVBUF) || (optname == SCE_NET_SO_SNDBUF)))
            value = 131072;
    }

    std::memcpy(optval, &value, sizeof(value));
    *optlen = sizeof(value);
    return 0;
}

int P2PMuxSocket::connect(const SceNetSockaddr *addr, unsigned int addrlen) {
    return SCE_NET_ERROR_EOPNOTSUPP;
}

SocketPtr P2PMuxSocket::accept(SceNetSockaddr *addr, unsigned int *addrlen, int &err) {
    err = SCE_NET_ERROR_EOPNOTSUPP;
    return nullptr;
}

int P2PMuxSocket::listen(int backlog) {
    return SCE_NET_ERROR_EOPNOTSUPP;
}

int P2PMuxSocket::get_peer_address(SceNetSockaddr *addr, unsigned int *addrlen) {
    return SCE_NET_ERROR_ENOTCONN;
}

int P2PMuxSocket::get_socket_address(SceNetSockaddr *addr, unsigned int *addrlen) {
    if (!addr || !addrlen)
        return SCE_NET_ERROR_EINVAL;

    SceNetSockaddrIn addr_in{};
    addr_in.sin_len = sizeof(addr_in);
    addr_in.sin_family = SCE_NET_AF_INET;
    {
        const std::lock_guard<std::mutex> lock(mutex);
        if (port) {
            addr_in.sin_port = htons(port->port);
            addr_in.sin_addr.s_addr = port->bound_addr();
            addr_in.sin_vport = htons(vport);
        }
    }

    std::memcpy(addr, &addr_in, sizeof(addr_in));
    *addrlen = sizeof(addr_in);
    return 0;
}

unsigned int P2PMuxSocket::poll_events() {
    const std::lock_guard<std::mutex> lock(mutex);
    return (queue.empty() ? 0 : SCE_NET_EPOLLIN) | SCE_NET_EPOLLOUT;
}

bool P2PMuxSocket::is_reusable() {
    const std::lock_guard<std::mutex> lock(mutex);
    return sockopt_so_reuseaddr || sockopt_so_reuseport;
}

void P2PMuxSocket::push_datagram(P2PDatagram &&dgram) {
    {
        const std::lock_guard<std::mutex> lock(mutex);
        if (closed)
            return;
        if (queue.size() >= P2P_MAX_QUEUED_DATAGRAMS) {
            LOG_WARN_ONCE("Console P2P: receive queue of vport {} is full, dropping the oldest datagrams", vport);
            queue.pop_front();
        }
        queue.push_back(std::move(dgram));
    }
    cond.notify_all();
}

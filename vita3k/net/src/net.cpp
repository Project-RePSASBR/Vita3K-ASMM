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

#include <net/state.h>

#ifdef _WIN32
#include <WinSock2.h>
#endif

void NetState::apply_p2p_address_override() {
    if (!console_p2p || !p2p)
        return;

    if (p2p->bind_addr != 0) {
        netAddr = p2p->bind_addr;
        // A loopback bind can't reach a subnet broadcast, only the limited broadcast address
        if ((ntohl(p2p->bind_addr) >> 24) == 127)
            broadcastAddr = INADDR_BROADCAST;
    }
    p2p->broadcast_addr = broadcastAddr;
}

void NetState::abort_all() {
    for (auto &[id, sock] : socks)
        sock->abort(SCE_NET_SOCKET_ABORT_FLAG_RCV_PRESERVATION | SCE_NET_SOCKET_ABORT_FLAG_SND_PRESERVATION);
    for (auto &[id, epoll] : epolls)
        epoll->aborted = true;
}

void NetState::deinit() {
    // Joins the hello thread, before the console P2P context goes away
    psas_hello.reset();

    for (auto &[id, sock] : socks) {
        sock->close();
    }
    socks.clear();
    epolls.clear();
    if (p2p) {
        p2p->shutdown();
        p2p.reset();
    }
    console_p2p = false;

#ifdef _WIN32
    if (inited)
        WSACleanup();
#endif

    inited = false;
    next_id = 0;
    next_epoll_id = 0;
    state = -1;
    resolver_id = 0;
    current_addr_index = 0;
    broadcastAddr = 0xFFFFFFFF;
    netAddr = 0xFFFFFFFF;
}

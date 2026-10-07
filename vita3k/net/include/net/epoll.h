#pragma once

#include <net/socket.h>

#include <atomic>

struct EpollSocket {
    unsigned int events;
    SceNetEpollData data;
    std::weak_ptr<Socket> sock;
};

struct Epoll {
    std::map<int, EpollSocket> eventEntries;
    // Set when the emulator stops, so a wait on console P2P sockets can't block shutdown
    std::atomic<bool> aborted = false;

    int add(int id, std::weak_ptr<Socket> sock, SceNetEpollEvent *ev);
    int del(int id);
    int mod(int id, SceNetEpollEvent *ev);
    int wait(SceNetEpollEvent *events, int maxevents, int timeout);

private:
    int wait_with_p2p(SceNetEpollEvent *events, int maxevents, int timeout);
};

typedef std::shared_ptr<Epoll> EpollPtr;

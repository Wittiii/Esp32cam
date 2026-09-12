#pragma once
#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <string>
#include <vector>
#include <sys/types.h>

using String = std::string;
using u_short = unsigned short;
using IPPORT = uint16_t;
using IPADDRESS = uint32_t;
struct SocketState {
    std::string incoming, outgoing;
    bool connected = true;
    size_t readChunk = 4096;
    unsigned writes = 0, failOnWrite = 0, writeMs = 0;
};
struct FakeSocket { std::shared_ptr<SocketState> state; };
using SOCKET = FakeSocket *;
using UDPSOCKET = int *;
#define NULLSOCKET nullptr
inline uint32_t testMillis = 1;
inline unsigned udpSockets = 0;
inline unsigned udpAttempts = 0;
inline bool failUdp = false;
inline uint32_t millis() { return testMillis; }
#define getRandom() 42
inline void closesocket(SOCKET socket) { socket->state->connected = false; delete socket; }
inline int socketread(SOCKET socket, char *data, size_t size, uint32_t) {
    auto &state = *socket->state;
    if (!state.connected) return 0;
    const size_t count = std::min(size, std::min(state.readChunk, state.incoming.size()));
    if (!count) return -1;
    memcpy(data, state.incoming.data(), count);
    state.incoming.erase(0, count);
    return int(count);
}
inline ssize_t socketsend(SOCKET socket, const void *data, size_t size) {
    auto &state = *socket->state;
    testMillis += state.writeMs;
    if (++state.writes == state.failOnWrite) return -1;
    state.outgoing.append(static_cast<const char *>(data), size);
    return ssize_t(size);
}
inline ssize_t sockettrysend(SOCKET socket, const void *data, size_t size) { return socketsend(socket, data, size); }
inline UDPSOCKET udpsocketcreate(unsigned short) {
    ++udpAttempts;
    if (failUdp) return nullptr;
    ++udpSockets;
    return new int(0);
}
inline void udpsocketclose(UDPSOCKET socket) { if (socket) { --udpSockets; delete socket; } }
inline void socketpeeraddr(SOCKET, IPADDRESS *address, IPPORT *port) { *address = 1; *port = 5000; }
inline ssize_t udpsocketsend(UDPSOCKET, const void *, size_t size, IPADDRESS, IPPORT) { return ssize_t(size); }

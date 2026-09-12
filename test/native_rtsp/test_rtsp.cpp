#include "CRtspSession.h"
#include <cassert>
#include <iostream>

class Streamer : public CStreamer {
public:
    Streamer() : CStreamer(320, 240) {}
    void streamImage(uint32_t) override {}
    void send(const std::vector<uint8_t> &jpeg) { streamFrame(jpeg.data(), jpeg.size(), millis()); }
};

std::vector<uint8_t> image() {
    std::vector<uint8_t> data{0xff, 0xd8, 0xff, 0xdb, 0, 132, 0};
    data.insert(data.end(), 64, 8);
    data.push_back(1);
    data.insert(data.end(), 64, 9);
    data.insert(data.end(), {0xff, 0xda, 0, 12, 3, 1, 0, 2, 0x11, 3, 0x11, 0, 63, 0});
    data.insert(data.end(), 5000, 0x22);
    data.insert(data.end(), {0xff, 0x00, 0x33, 0xff, 0xd9});
    return data;
}

std::string request(const std::string &method, unsigned cseq, const std::string &extra = "") {
    return method + " rtsp://camera/mjpeg/1 RTSP/1.0\r\nCSeq: " + std::to_string(cseq) + "\r\n" + extra + "\r\n";
}

std::shared_ptr<SocketState> connect(Streamer &streamer, CRtspSession *&session) {
    auto state = std::make_shared<SocketState>();
    session = streamer.addSession(new FakeSocket{state});
    return state;
}

void setup(CRtspSession *session, const std::shared_ptr<SocketState> &state, const std::string &transport) {
    state->incoming = request("SETUP", 2, "Transport: " + transport + "\r\n");
    session->handleRequests(0);
    assert(!session->m_stopped);
}

void jpegBounds() {
    const auto data = image();
    const uint8_t *scan = data.data(), *q0, *q1;
    uint32_t length = data.size();
    assert(decodeJPEGfile(&scan, &length, &q0, &q1));
    assert(length == 5005 && q0[0] == 8 && q1[0] == 9);
    for (size_t truncated = 0; truncated < data.size(); ++truncated) {
        scan = data.data(); length = truncated;
        assert(!decodeJPEGfile(&scan, &length, &q0, &q1));
    }
    auto bad = data;
    bad[4] = 0xff; bad[5] = 0xff;
    scan = bad.data(); length = bad.size();
    assert(!decodeJPEGfile(&scan, &length, &q0, &q1));
}

void fragmentPipelineAndRtcp() {
    Streamer streamer;
    CRtspSession *session;
    auto state = connect(streamer, session);
    state->readChunk = 1;
    state->incoming = "OPTIONS * RTSP/1.0\r\ncseq: 1\r\n\r\n";
    while (!state->incoming.empty()) session->handleRequests(0);
    assert(state->outgoing.find("CSeq: 1") != std::string::npos);
    state->readChunk = 4096;
    state->incoming = request("SETUP", 2, "Transport: RTP/AVP/TCP;unicast;interleaved=2-3\r\n") + request("PLAY", 3);
    session->handleRequests(0);
    assert(session->m_streaming && session->getRtpChannel() == 2);
    assert(state->outgoing.find("interleaved=2-3") != std::string::npos);
    state->incoming = std::string("$\3\0\4\0\r\n\0", 8) + request("GET_PARAMETER", 4);
    session->handleRequests(0);
    assert(!session->m_stopped && state->outgoing.find("CSeq: 4") != std::string::npos);
    state->outgoing.clear();
    streamer.send(image());
    assert(state->outgoing[0] == '$' && state->outgoing[1] == 2);
    size_t fragments = 0;
    for (size_t pos = 0; pos < state->outgoing.size();) {
        const auto *packet = reinterpret_cast<const uint8_t *>(state->outgoing.data() + pos);
        assert(packet[0] == '$' && packet[1] == 2 && packet[21] == 128);
        pos += 4 + (unsigned(packet[2]) << 8) + packet[3];
        assert(pos <= state->outgoing.size());
        ++fragments;
    }
    assert(fragments > 1);
    state->incoming = request("PAUSE", 5) + request("TEARDOWN", 6);
    session->handleRequests(0);
    assert(!session->m_streaming && session->m_stopped);
}

void timestampTracksCurrentFrameAndRollover() {
    for (uint32_t start : {0U, UINT32_MAX - 20U}) {
        Streamer streamer;
        CRtspSession *session;
        auto state = connect(streamer, session);
        setup(session, state, "RTP/AVP/TCP;unicast;interleaved=0-1");
        session->m_streaming = true;
        testMillis = start;
        for (unsigned frame = 0; frame < 3; ++frame) {
            state->outgoing.clear();
            streamer.send(image());
            const auto *packet = reinterpret_cast<const uint8_t *>(state->outgoing.data());
            const uint32_t stamp = (uint32_t(packet[8]) << 24) | (uint32_t(packet[9]) << 16) |
                                   (uint32_t(packet[10]) << 8) | packet[11];
            assert(stamp == frame * 40U * 90U);
            testMillis += 40;
        }
    }
}

void rejectsBadRequests() {
    const std::vector<std::string> badRequests{
        "DESCRIBE rtsp://" + std::string(256, 'a') + "/mjpeg/1 RTSP/1.0\r\nCSeq: 1\r\n\r\n",
        "OPTIONS * RTSP/1.0\r\nCSeq: 99999999999999999999\r\n\r\n",
        "OPTIONS * RTSP/1.0\r\nCSeq: 1\r\nContent-Length: 9\r\n\r\n",
        request("SETUP", 2, "Transport: RTP/AVP;client_port=999999-1\r\n"),
        std::string(4095, 'x'),
    };
    for (const auto &bad : badRequests) {
        Streamer streamer;
        CRtspSession *session;
        auto state = connect(streamer, session);
        state->incoming = bad;
        session->handleRequests(0);
        assert(session->m_stopped);
    }
    Streamer streamer;
    CRtspSession *session;
    auto state = connect(streamer, session);
    state->incoming = "OPTIONS rtsp://camera/";
    session->handleRequests(0);
    testMillis += 5001;
    session->handleRequests(0);
    assert(session->m_stopped);
}

void disconnectDoesNotHangAndFrameBudget() {
    for (bool slow : {false, true}) {
        Streamer streamer;
        CRtspSession *session;
        auto state = connect(streamer, session);
        setup(session, state, "RTP/AVP/TCP;unicast;interleaved=0-1");
        session->m_streaming = true;
        state->writes = 0;
        if (slow) state->writeMs = 100;
        else state->failOnWrite = 2;
        const uint32_t start = millis();
        streamer.send(image());
        assert(state->writes <= 3);
        assert(millis() - start <= 300);
        if (!slow) assert(session->m_stopped);
    }
}

void udpOwnershipAndBoundedBind() {
    Streamer streamer;
    CRtspSession *udp, *tcp;
    auto udpState = connect(streamer, udp);
    setup(udp, udpState, "RTP/AVP;unicast;client_port=5000-5001");
    auto tcpState = connect(streamer, tcp);
    setup(tcp, tcpState, "RTP/AVP/TCP;unicast;interleaved=0-1");
    assert(udpSockets == 2);
    delete tcp;
    assert(udpSockets == 2);
    setup(udp, udpState, "RTP/AVP;unicast;client_port=6000-6001");
    assert(udpSockets == 2);
    delete udp;
    assert(udpSockets == 0);
    failUdp = true;
    udpAttempts = 0;
    assert(!streamer.InitUdpTransport());
    assert(udpAttempts == 16);
    failUdp = false;
}

void sessionMemoryIsBounded() {
    Streamer streamer;
    for (int i = 0; i < CStreamer::kMaxSessions; ++i) {
        CRtspSession *session;
        connect(streamer, session);
        assert(session != nullptr);
    }
    CRtspSession *rejected;
    auto state = connect(streamer, rejected);
    assert(rejected == nullptr && !state->connected);
    assert(streamer.sessionCount() == CStreamer::kMaxSessions);
}

int main() {
    jpegBounds();
    fragmentPipelineAndRtcp();
    rejectsBadRequests();
    disconnectDoesNotHangAndFrameBudget();
    udpOwnershipAndBoundedBind();
    timestampTracksCurrentFrameAndRollover();
    sessionMemoryIsBounded();
    std::cout << "7 RTSP regression groups passed\n";
}

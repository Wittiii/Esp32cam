#include "CRtspSession.h"
#include <cctype>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <climits>

//===========================================================
//===========================================================
//===========================================================
CRtspSession::CRtspSession(SOCKET aClient, CStreamer * aStreamer) : LinkedListElement(aStreamer->getClientsListHead()),
    m_Client(aClient),
    m_Streamer(aStreamer)
{
    printf("Creating RTSP session\n");
    newCommandInit();

    m_RtspClient = m_Client;
    m_RtspSessionID  = getRandom();         // create a session ID
    m_RtspSessionID |= 0x80000000;
    m_StreamID       = -1;
    m_ClientRTPPort  =  0;
    m_ClientRTCPPort =  0;
    m_TcpTransport   =  false;
    m_streaming = false;
    m_stopped = false;

    m_RtpClientPort  = 0;
    m_RtcpClientPort = 0;

    m_CSeq = 0; // CSeq sequense must be kept through the whole session
    m_RtspCmdType = RTSP_UNKNOWN;
    m_RecvBufPos = 0;
    m_HeaderState = headerUnknown;
    m_connectedAt = millis();
    memset(m_RecvBuf, 0x00, sizeof(m_RecvBuf));
    debug = false;
}

CRtspSession::~CRtspSession()
{
    if (m_UdpInitialized) m_Streamer->ReleaseUdpTransport();
    closesocket(m_RtspClient);
}

/*! @brief Initialize stuff for processing new client's command */
void CRtspSession::newCommandInit()
{
    memset( m_CommandPresentationPart, 0x00, sizeof( m_CommandPresentationPart ) );
    memset( m_CommandStreamPart,    0x00, sizeof( m_CommandStreamPart ) );
    memset( m_CommandHostPort,  0x00, sizeof( m_CommandHostPort ) );
    m_ContentLength = 0;
}

namespace {
bool readUnsigned(const char *value, unsigned maximum, unsigned &result) {
    while (*value == ' ' || *value == '\t') ++value;
    if (*value < '0' || *value > '9') return false;
    result = 0;
    do {
        const unsigned digit = unsigned(*value++ - '0');
        if (digit > maximum || result > (maximum - digit) / 10) return false;
        result = result * 10 + digit;
    } while (*value >= '0' && *value <= '9');
    while (*value == ' ' || *value == '\t') ++value;
    return *value == '\0';
}

bool readPair(char *value, unsigned maximum, unsigned &first, unsigned &second) {
    char *dash = strchr(value, '-');
    if (dash) *dash++ = '\0';
    if (!readUnsigned(value, maximum, first)) return false;
    if (dash) return readUnsigned(dash, maximum, second);
    if (first == maximum) return false;
    second = first + 1;
    return true;
}
}

// The caller passes exactly one complete header, with room for its trailing NUL.
// Parse in place; every string boundary is inside the fixed receive buffer.
bool CRtspSession::ParseRtspRequest(char *request, unsigned size) {
    if (!request || size < 4 || size >= RTSP_BUFFER_SIZE ||
        memcmp(request + size - 4, "\r\n\r\n", 4) != 0 ||
        memchr(request, '\0', size) != nullptr) return false;
    newCommandInit();
    request[size - 2] = '\0';
    char *line = request;
    if (line[0] == '\r' && line[1] == '\n') line += 2;
    char *lineEnd = strstr(line, "\r\n");
    if (!lineEnd) return false;
    *lineEnd = '\0';
    char *uri = strchr(line, ' ');
    if (!uri) return false;
    *uri++ = '\0';
    while (*uri == ' ') ++uri;
    char *version = strchr(uri, ' ');
    if (!version) return false;
    *version++ = '\0';
    while (*version == ' ') ++version;
    if (strcmp(version, "RTSP/1.0") != 0) return false;

    if (!strcmp(line, "OPTIONS")) m_RtspCmdType = RTSP_OPTIONS;
    else if (!strcmp(line, "DESCRIBE")) m_RtspCmdType = RTSP_DESCRIBE;
    else if (!strcmp(line, "SETUP")) m_RtspCmdType = RTSP_SETUP;
    else if (!strcmp(line, "PLAY")) m_RtspCmdType = RTSP_PLAY;
    else if (!strcmp(line, "PAUSE")) m_RtspCmdType = RTSP_PAUSE;
    else if (!strcmp(line, "TEARDOWN")) m_RtspCmdType = RTSP_TEARDOWN;
    else if (!strcmp(line, "GET_PARAMETER")) m_RtspCmdType = RTSP_GET_PARAMETER;
    else return false;

    const bool genericUri = m_RtspCmdType == RTSP_OPTIONS || m_RtspCmdType == RTSP_GET_PARAMETER;
    if (strcmp(uri, "*") == 0) {
        if (!genericUri) return false;
    } else {
        if (strncasecmp(uri, "rtsp://", 7) != 0) return false;
        char *host = uri + 7;
        char *presentation = strchr(host, '/');
        if (presentation) *presentation++ = '\0';
        if (!*host || strlen(host) >= sizeof(m_CommandHostPort)) return false;
        strcpy(m_CommandHostPort, host);
        char *stream = presentation ? strchr(presentation, '/') : nullptr;
        if (stream) *stream++ = '\0';
        if (!genericUri && (!presentation || !*presentation || !stream || !*stream)) return false;
        if (presentation) {
            if (strlen(presentation) >= sizeof(m_CommandPresentationPart)) return false;
            strcpy(m_CommandPresentationPart, presentation);
        }
        if (stream) {
            size_t streamLength = strlen(stream);
            while (streamLength && stream[streamLength - 1] == '/') stream[--streamLength] = '\0';
            if (strchr(stream, '/') || streamLength >= sizeof(m_CommandStreamPart)) return false;
            strcpy(m_CommandStreamPart, stream);
        }
    }

    bool hasCseq = false, hasTransport = false;
    for (line = lineEnd + 2; *line;) {
        lineEnd = strstr(line, "\r\n");
        if (!lineEnd) return false;
        while (lineEnd[2] == ' ' || lineEnd[2] == '\t') {
            lineEnd[0] = lineEnd[1] = ' ';
            lineEnd = strstr(lineEnd + 2, "\r\n");
            if (!lineEnd) return false;
        }
        *lineEnd = '\0';
        char *value = strchr(line, ':');
        if (!value) return false;
        *value++ = '\0';
        while (*value == ' ' || *value == '\t') ++value;
        if (!strcasecmp(line, "CSeq")) {
            if (hasCseq || !readUnsigned(value, UINT_MAX, m_CSeq)) return false;
            hasCseq = true;
        } else if (!strcasecmp(line, "Content-Length")) {
            if (!readUnsigned(value, RTSP_BUFFER_SIZE - 1, m_ContentLength) || m_ContentLength != 0) return false;
        } else if (!strcasecmp(line, "Transport") && m_RtspCmdType == RTSP_SETUP) {
            if (hasTransport) return false;
            hasTransport = true;
            char *part = strchr(value, ';');
            if (part) *part++ = '\0';
            if (!strcmp(value, "RTP/AVP/TCP")) m_TcpTransport = true;
            else if (!strcmp(value, "RTP/AVP") || !strcmp(value, "RTP/AVP/UDP")) m_TcpTransport = false;
            else return false;
            m_ClientRTPPort = m_ClientRTCPPort = 0;
            m_RtpChannel = 0; m_RtcpChannel = 1;
            while (part && *part) {
                char *next = strchr(part, ';');
                if (next) *next++ = '\0';
                while (*part == ' ' || *part == '\t') ++part;
                unsigned first, second;
                if (!strncmp(part, "client_port=", 12)) {
                    if (!readPair(part + 12, 65535, first, second) || first == 0 || second == 0) return false;
                    m_ClientRTPPort = first; m_ClientRTCPPort = second;
                } else if (!strncmp(part, "interleaved=", 12)) {
                    if (!readPair(part + 12, 255, first, second) || first == second) return false;
                    m_RtpChannel = first; m_RtcpChannel = second;
                } else if (!strcmp(part, "multicast")) return false;
                part = next;
            }
            if (!m_TcpTransport && !m_ClientRTPPort) return false;
        }
        line = lineEnd + 2;
    }
    return hasCseq && (m_RtspCmdType != RTSP_SETUP || hasTransport);
}

RTSP_CMD_TYPES CRtspSession::Handle_RtspRequest( char *aRequest, unsigned aRequestSize )
{
    if ( !ParseRtspRequest( aRequest, aRequestSize ) ) {
        return RTSP_UNKNOWN;
    }

    switch ( m_RtspCmdType )
    {
        case RTSP_OPTIONS:  Handle_RtspOPTION();   break;
        case RTSP_DESCRIBE: Handle_RtspDESCRIBE(); break;
        case RTSP_SETUP:    Handle_RtspSETUP();    break;
        case RTSP_PLAY:
            if (!m_transportReady) { sendSimpleResponse(455, "Method Not Valid in This State"); m_stopped = true; return RTSP_UNKNOWN; }
            Handle_RtspPLAY(); break;
        case RTSP_PAUSE:
        case RTSP_GET_PARAMETER:
        case RTSP_TEARDOWN: sendSimpleResponse(200, "OK"); break;
        default: break;
    }

    return m_RtspCmdType;
}

void CRtspSession::Handle_RtspOPTION()
{
    static char Response[1024]; // Note: we assume single threaded, this large buf we keep off of the tiny stack

    snprintf(Response,sizeof(Response),
             "RTSP/1.0 200 OK\r\nCSeq: %u\r\n"
             "Public: OPTIONS, DESCRIBE, SETUP, TEARDOWN, PLAY, PAUSE, GET_PARAMETER\r\n\r\n", m_CSeq);

    socketsend(m_RtspClient,Response,strlen(Response));
}

void CRtspSession::Handle_RtspDESCRIBE() // FIXME: too much redundancy. should eliminate intermediate buffers.
{
    static char Response[1024]; // Note: we assume single threaded, this large buf we keep off of the tiny stack
    static char SDPBuf[1024];
    static char URLBuf[1024];

    // check whether we know a stream with the URL which is requested
    m_StreamID = -1;        // invalid URL

    if ( m_Streamer->getURIPresentation() == m_CommandPresentationPart &&
            m_Streamer->getURIStream() == m_CommandStreamPart )
        m_StreamID = 0;

    if ( m_StreamID == -1 )
    {   // Stream not available
        snprintf( Response, sizeof(Response),
                 "RTSP/1.0 404 Stream Not Found\r\nCSeq: %u\r\n%s\r\n",
                 m_CSeq,
                 DateHeader());

        socketsend( m_RtspClient, Response, strlen(Response) );
        return;
    }

    // simulate DESCRIBE server response
    static char OBuf[256];
    char * ColonPtr;
    strcpy( OBuf, m_CommandHostPort );
    ColonPtr = strstr( OBuf, ":" );
    if (ColonPtr != nullptr) ColonPtr[0] = 0x00;

    snprintf( SDPBuf, sizeof(SDPBuf),
             "v=0\r\n"
             "o=- %d 1 IN IP4 %s\r\n"
             "s=ESP32 Camera\r\n"
             "t=0 0\r\n"                                       // start / stop - 0 -> unbounded and permanent session
             "m=video 0 RTP/AVP 26\r\n"                        // currently we just handle UDP sessions (??????)
             "c=IN IP4 0.0.0.0\r\n"
             "a=rtpmap:26 JPEG/90000\r\n",
             rand(),
             OBuf );

    snprintf( URLBuf, sizeof(URLBuf),
             "rtsp://%s/%s/%s", m_CommandHostPort, m_CommandPresentationPart, m_CommandStreamPart );

    snprintf( Response, sizeof(Response),
             "RTSP/1.0 200 OK\r\nCSeq: %u\r\n"
             "%s\r\n"
             "Content-Base: %s/\r\n"
             "Content-Type: application/sdp\r\n"
             "Content-Length: %d\r\n\r\n"
             "%s",
             m_CSeq,
             DateHeader(),
             URLBuf,
             (int) strlen(SDPBuf),
             SDPBuf);

    socketsend( m_RtspClient, Response, strlen(Response) );
}

bool CRtspSession::InitTransport(u_short aRtpPort, u_short aRtcpPort)
{
    if (m_UdpInitialized) m_Streamer->ReleaseUdpTransport();
    m_UdpInitialized = false;
    m_transportReady = false;
    m_RtpClientPort  = aRtpPort;
    m_RtcpClientPort = aRtcpPort;

    if (!m_TcpTransport)
    {   // allocate port pairs for RTP/RTCP ports in UDP transport mode
        if (!m_Streamer->InitUdpTransport()) return false;
        m_UdpInitialized = true;
    };
    m_transportReady = true;
    return true;
};

void CRtspSession::Handle_RtspSETUP()
{
    static char Response[1024];
    static char Transport[255];

    // init RTSP Session transport type (UDP or TCP) and ports for UDP transport
    if (!InitTransport(m_ClientRTPPort,m_ClientRTCPPort)) {
        sendSimpleResponse(461, "Unsupported Transport");
        m_stopped = true;
        return;
    }

    // simulate SETUP server response
    if (m_TcpTransport)
        snprintf(Transport,sizeof(Transport),"RTP/AVP/TCP;unicast;interleaved=%u-%u", m_RtpChannel, m_RtcpChannel);
    else
        snprintf(Transport,sizeof(Transport),
                 "RTP/AVP;unicast;destination=127.0.0.1;source=127.0.0.1;client_port=%i-%i;server_port=%i-%i",
                 m_ClientRTPPort,
                 m_ClientRTCPPort,
                 m_Streamer->GetRtpServerPort(),
                 m_Streamer->GetRtcpServerPort());
    snprintf(Response,sizeof(Response),
             "RTSP/1.0 200 OK\r\nCSeq: %u\r\n"
             "%s\r\n"
             "Transport: %s\r\n"
             "Session: %i\r\n\r\n",
             m_CSeq,
             DateHeader(),
             Transport,
             m_RtspSessionID);

    socketsend(m_RtspClient,Response,strlen(Response));
}

void CRtspSession::Handle_RtspPLAY()
{
    static char Response[1024];

    // simulate SETUP server response
    snprintf( Response, sizeof(Response),
             "RTSP/1.0 200 OK\r\nCSeq: %u\r\n"
             "%s\r\n"
             "Range: npt=0.000-\r\n"
             "Session: %i\r\n"
             "RTP-Info: url=rtsp://127.0.0.1:8554/mjpeg/1/track1\r\n\r\n", // FIXME
             m_CSeq,
             DateHeader(),
             m_RtspSessionID);

    socketsend(m_RtspClient,Response,strlen(Response));
}

void CRtspSession::sendSimpleResponse(unsigned status, const char *reason)
{
    char response[192];
    const int size = snprintf(response, sizeof(response), "RTSP/1.0 %u %s\r\nCSeq: %u\r\nSession: %i\r\n\r\n", status, reason, m_CSeq, m_RtspSessionID);
    if (socketsend(m_RtspClient, response, size) != size) m_stopped = true;
}

char const * CRtspSession::DateHeader()
{
    static char buf[200];
    time_t tt = time(NULL);
    strftime(buf, sizeof buf, "Date: %a, %b %d %Y %H:%M:%S GMT", gmtime(&tt));
    return buf;
}

int CRtspSession::GetStreamID()
{
    return m_StreamID;
};

/**
   Read from our socket, parsing commands as possible.
 */
bool CRtspSession::handleRequests(uint32_t readTimeoutMs) {
    if (m_stopped) return false;
    const uint32_t now = millis();
    if (((m_RecvBufPos || m_interleavedRemaining) && now - m_partialStartedAt >= 5000) ||
        (!m_transportReady && now - m_connectedAt >= 10000)) {
        m_stopped = true;
        return false;
    }
    const int received = socketread(m_RtspClient, m_RecvBuf + m_RecvBufPos,
        sizeof(m_RecvBuf) - m_RecvBufPos - 1, readTimeoutMs);
    if (received == 0) { m_stopped = true; return false; }
    if (received > 0) {
        if (!m_RecvBufPos && !m_interleavedRemaining) m_partialStartedAt = now;
        m_RecvBufPos += received;
    }
    m_RecvBuf[m_RecvBufPos] = '\0';
    auto consume = [this, now](size_t count) {
        m_RecvBufPos -= count;
        memmove(m_RecvBuf, m_RecvBuf + count, m_RecvBufPos);
        m_RecvBuf[m_RecvBufPos] = '\0';
        m_partialStartedAt = now;
    };
    // Bound work per camera-loop iteration, preserving any pipelined remainder.
    for (unsigned handled = 0; m_RecvBufPos && handled < 4; ++handled) {
        if (m_interleavedRemaining) {
            size_t count = m_interleavedRemaining < m_RecvBufPos ? m_interleavedRemaining : m_RecvBufPos;
            m_interleavedRemaining -= count;
            consume(count);
            continue;
        }
        if (m_RecvBuf[0] == '$') {
            if (m_RecvBufPos < 4) break;
            m_interleavedRemaining = (uint8_t(m_RecvBuf[2]) << 8) | uint8_t(m_RecvBuf[3]);
            consume(4);
            continue; // RTCP is binary, including NULs and CRLF, not an RTSP command.
        }
        const char *end = strstr(m_RecvBuf, "\r\n\r\n");
        if (!end) {
            if (m_RecvBufPos == sizeof(m_RecvBuf) - 1 || memchr(m_RecvBuf, '\0', m_RecvBufPos)) {
                m_stopped = true;
                sendSimpleResponse(400, "Bad Request");
            }
            break;
        }
        const size_t headerLength = end - m_RecvBuf + 4;
        const RTSP_CMD_TYPES command = Handle_RtspRequest(m_RecvBuf, headerLength);
        if (command == RTSP_UNKNOWN) {
            if (!m_stopped) sendSimpleResponse(400, "Bad Request");
            m_stopped = true;
        } else if (command == RTSP_PLAY) m_streaming = !m_stopped;
        else if (command == RTSP_PAUSE) m_streaming = false;
        else if (command == RTSP_TEARDOWN) m_stopped = true;
        consume(headerLength);
        if (m_stopped) break;
    }
    return received > 0 && !m_stopped;
}

#include "CStreamer.h"
#include "CRtspSession.h"

#include <stdio.h>
#include <new>

CStreamer::CStreamer(u_short width, u_short height) : m_Clients()
{
    printf("Creating TSP streamer\n");
    m_RtpServerPort  = 0;
    m_RtcpServerPort = 0;

    m_SequenceNumber = 0;
    m_Timestamp      = 0;
    m_SendIdx        = 0;

    m_RtpSocket = NULLSOCKET;
    m_RtcpSocket = NULLSOCKET;

    m_width = width;
    m_height = height;
    m_prevMsec = 0;
    m_timestampInitialized = false;

    m_udpRefCount = 0;
    m_nonBlockingTcpWrites = false;
    m_serviceCallback = NULL;

    debug = false;

    m_URIHost = "127.0.0.1:554";
    m_URIPresentation = "mjpeg";
    m_URIStream = "1";
}

CStreamer::~CStreamer()
{
    LinkedListElement* element = m_Clients.m_Next;
    CRtspSession* session = NULL;
    while (element != &m_Clients)
    {
        session = static_cast<CRtspSession*>(element);
        element = element->m_Next;
        delete session;
    }
};

CRtspSession* CStreamer::addSession( SOCKET aClient )
{
    // Every session owns a receive buffer. Bound memory use independently of
    // how quickly clients send SETUP, PLAY or keep-alive messages.
    if (!aClient) return nullptr;
    if (sessionCount() >= kMaxSessions) {
        closesocket(aClient);
        return nullptr;
    }
    CRtspSession* session = new (std::nothrow) CRtspSession( aClient, this );
    if (!session) {
        closesocket(aClient);
        return nullptr;
    }
    // we have it stored in m_Clients
    session->debug = debug;
    return session;
}

int CStreamer::sessionCount() const
{
    int count = 0;
    LinkedListElement* element = m_Clients.m_Next;
    while (element != &m_Clients)
    {
        CRtspSession* session = static_cast<CRtspSession*>(element);
        if (!session->m_stopped)
        {
            ++count;
        }
        element = element->m_Next;
    }
    return count;
}

int CStreamer::streamingSessionCount() const
{
    int count = 0;
    LinkedListElement* element = m_Clients.m_Next;
    while (element != &m_Clients)
    {
        CRtspSession* session = static_cast<CRtspSession*>(element);
        if (!session->m_stopped && session->m_streaming)
        {
            ++count;
        }
        element = element->m_Next;
    }
    return count;
}

void CStreamer::setURI( String hostport, String pres, String stream ) // set URI parts for sessions to use.
{
    m_URIHost = hostport;
    m_URIPresentation = pres;
    m_URIStream = stream;
}

int CStreamer::SendRtpPacket(unsigned const char * jpeg, int jpegLen, int fragmentOffset, BufPtr quant0tbl, BufPtr quant1tbl)
{
    // if ( debug ) printf("CStreamer::SendRtpPacket offset:%d - begin\n", fragmentOffset);
#define KRtpHeaderSize 12           // size of the RTP header
#define KJpegHeaderSize 8           // size of the special JPEG payload header

#define MAX_FRAGMENT_SIZE 1100 // FIXME, pick more carefully
    int fragmentLen = MAX_FRAGMENT_SIZE;
    if(fragmentLen + fragmentOffset > jpegLen) // Shrink last fragment if needed
        fragmentLen = jpegLen - fragmentOffset;

    bool isLastFragment = (fragmentOffset + fragmentLen) == jpegLen;

    if (streamingSessionCount() == 0)
    {
        return 0; // Returning the same offset loops forever after a disconnect.
    }

    // Do we have custom quant tables? If so include them per RFC

    bool includeQuantTbl = quant0tbl && quant1tbl && fragmentOffset == 0;
    // Q identifies the frame's tables in every fragment, even though only
    // fragment zero carries their bytes (RFC 2435 section 3.1.8).
    uint8_t q = quant0tbl && quant1tbl ? 128 : 0x5e;

    static char RtpBuf[2048]; // Note: we assume single threaded, this large buf we keep off of the tiny stack
    int RtpPacketSize = fragmentLen + KRtpHeaderSize + KJpegHeaderSize + (includeQuantTbl ? (4 + 64 * 2) : 0);

    // Prepare the first 4 byte of the packet. This is the Rtp over Rtsp header in case of TCP based transport
    RtpBuf[0]  = '$';        // magic number
    RtpBuf[1]  = 0;          // number of multiplexed subchannel on RTPS connection - here the RTP channel
    RtpBuf[2]  = (RtpPacketSize & 0x0000FF00) >> 8;
    RtpBuf[3]  = (RtpPacketSize & 0x000000FF);
    // Prepare the 12 byte RTP header
    RtpBuf[4]  = 0x80;                               // RTP version
    RtpBuf[5]  = 0x1a | (isLastFragment ? 0x80 : 0x00);                               // JPEG payload (26) and marker bit
    RtpBuf[7]  = m_SequenceNumber & 0x0FF;           // each packet is counted with a sequence counter
    RtpBuf[6]  = m_SequenceNumber >> 8;
    RtpBuf[8]  = (m_Timestamp & 0xFF000000) >> 24;   // each image gets a timestamp
    RtpBuf[9]  = (m_Timestamp & 0x00FF0000) >> 16;
    RtpBuf[10] = (m_Timestamp & 0x0000FF00) >> 8;
    RtpBuf[11] = (m_Timestamp & 0x000000FF);
    RtpBuf[12] = 0x13;                               // 4 byte SSRC (sychronization source identifier)
    RtpBuf[13] = 0xf9;                               // we just an arbitrary number here to keep it simple
    RtpBuf[14] = 0x7e;
    RtpBuf[15] = 0x67;

    // Prepare the 8 byte payload JPEG header
    RtpBuf[16] = 0x00;                               // type specific
    RtpBuf[17] = (fragmentOffset & 0x00FF0000) >> 16;                               // 3 byte fragmentation offset for fragmented images
    RtpBuf[18] = (fragmentOffset & 0x0000FF00) >> 8;
    RtpBuf[19] = (fragmentOffset & 0x000000FF);

    /*    These sampling factors indicate that the chrominance components of
       type 0 video is downsampled horizontally by 2 (often called 4:2:2)
       while the chrominance components of type 1 video are downsampled both
       horizontally and vertically by 2 (often called 4:2:0). */
    RtpBuf[20] = 0x00;                               // type (fixme might be wrong for camera data) https://tools.ietf.org/html/rfc2435
    RtpBuf[21] = q;                               // quality scale factor was 0x5e
    RtpBuf[22] = m_width / 8;                           // width  / 8
    RtpBuf[23] = m_height / 8;                           // height / 8

    int headerLen = 24; // Inlcuding jpeg header but not qant table header
    if(includeQuantTbl) { // we need a quant header - but only in first packet of the frame
        //if ( debug ) printf("inserting quanttbl\n");
        RtpBuf[24] = 0; // MBZ
        RtpBuf[25] = 0; // 8 bit precision
        RtpBuf[26] = 0; // MSB of lentgh

        int numQantBytes = 64; // Two 64 byte tables
        RtpBuf[27] = 2 * numQantBytes; // LSB of length

        headerLen += 4;

        memcpy(RtpBuf + headerLen, quant0tbl, numQantBytes);
        headerLen += numQantBytes;

        memcpy(RtpBuf + headerLen, quant1tbl, numQantBytes);
        headerLen += numQantBytes;
    }
    // if ( debug ) printf("Sending timestamp %d, seq %d, fragoff %d, fraglen %d, jpegLen %d\n", m_Timestamp, m_SequenceNumber, fragmentOffset, fragmentLen, jpegLen);

    // append the JPEG scan data to the RTP buffer
    memcpy(RtpBuf + headerLen,jpeg + fragmentOffset, fragmentLen);
    fragmentOffset += fragmentLen;

    m_SequenceNumber++;                              // prepare the packet counter for the next packet

    IPADDRESS otherip;
    IPPORT otherport;

    // RTP marker bit must be set on last fragment
    LinkedListElement* element = m_Clients.m_Next;
    CRtspSession* session = NULL;
    while (element != &m_Clients)
    {
        session = static_cast<CRtspSession*>(element);
        if (session->m_streaming && !session->m_stopped) {
            if (session->isTcpTransport()) // RTP over RTSP - we send the buffer + 4 byte additional header
            {
                RtpBuf[1] = session->getRtpChannel();
                const int packetLength = RtpPacketSize + 4;
                const ssize_t sent = m_nonBlockingTcpWrites
                    ? sockettrysend(session->getClient(), RtpBuf, packetLength)
                    : socketsend(session->getClient(), RtpBuf, packetLength);
                if (sent != packetLength) session->m_stopped = true;
            }
            else                // UDP - we send just the buffer by skipping the 4 byte RTP over RTSP header
            {
                socketpeeraddr(session->getClient(), &otherip, &otherport);
                udpsocketsend(m_RtpSocket,&RtpBuf[4],RtpPacketSize, otherip, session->getRtpClientPort());
            }
        }
        element = element->m_Next;
    }
    if (m_serviceCallback != NULL)
        m_serviceCallback();
    // if ( debug ) printf("CStreamer::SendRtpPacket offset:%d - end\n", fragmentOffset);
    return isLastFragment ? 0 : fragmentOffset;
};

u_short CStreamer::GetRtpServerPort()
{
    return m_RtpServerPort;
};

u_short CStreamer::GetRtcpServerPort()
{
    return m_RtcpServerPort;
};

bool CStreamer::InitUdpTransport(void)
{
    if (m_udpRefCount != 0)
    {
        ++m_udpRefCount;
        return true;
    }

    for (u_short P = 6970; P < 7002; P += 2)
    {
        m_RtpSocket     = udpsocketcreate(P);
        if (m_RtpSocket)
        {   // Rtp socket was bound successfully. Lets try to bind the consecutive Rtsp socket
            m_RtcpSocket = udpsocketcreate(P + 1);
            if (m_RtcpSocket)
            {
                m_RtpServerPort  = P;
                m_RtcpServerPort = P+1;
                m_udpRefCount = 1;
                return true;
            }
            else
            {
                udpsocketclose(m_RtpSocket);
                m_RtpSocket = NULLSOCKET;
            };
        }
    };
    return false;
}

void CStreamer::ReleaseUdpTransport(void)
{
    if (m_udpRefCount == 0)
        return;
    --m_udpRefCount;
    if (m_udpRefCount == 0)
    {
        m_RtpServerPort  = 0;
        m_RtcpServerPort = 0;
        udpsocketclose(m_RtpSocket);
        udpsocketclose(m_RtcpSocket);

        m_RtpSocket = NULLSOCKET;
        m_RtcpSocket = NULLSOCKET;
    }
}

/**
   Call handleRequests on all sessions
 */
bool CStreamer::handleRequests(uint32_t readTimeoutMs)
{
    bool retVal = true;
    LinkedListElement* element = m_Clients.m_Next;
    while(element != &m_Clients)
    {
        CRtspSession* session = static_cast<CRtspSession*>(element);
        retVal &= session->handleRequests(readTimeoutMs);

        element = element->m_Next;

        if (session->m_stopped) 
        {
            // remove session here, so we wont have to send to it
            delete session;
        }
    }

    return retVal;
}

void CStreamer::streamFrame(unsigned const char *data, uint32_t dataLen, uint32_t curMsec)
{
    // RTP timestamps belong to the current frame, not the preceding interval.
    // Unsigned subtraction also handles millis() rollover without a time jump.
    if (m_timestampInitialized) m_Timestamp += 90U * (curMsec - m_prevMsec);
    m_timestampInitialized = true;
    m_prevMsec = curMsec;

    // locate quant tables if possible
    BufPtr qtable0, qtable1;

    if(!decodeJPEGfile(&data, &dataLen, &qtable0, &qtable1)) {
        printf("can't decode jpeg data\n");
        return;
    }

    int offset = 0;
    const uint32_t sendStartedAt = millis();
    do {
        if (millis() - sendStartedAt >= 300 || streamingSessionCount() == 0) break;
        offset = SendRtpPacket(data, dataLen, offset, qtable0, qtable1);
    } while(offset != 0);

    m_SendIdx++;
    if (m_SendIdx > 1) m_SendIdx = 0;
};

// Parse each segment against the actual frame boundary; corrupt/truncated camera
// buffers must be dropped rather than read past PSRAM or scanned indefinitely.
bool decodeJPEGfile(BufPtr *start, uint32_t *len, BufPtr *qtable0, BufPtr *qtable1) {
    if (!start || !len || !qtable0 || !qtable1 || !*start || *len < 4) return false;
    const uint8_t *data = *start;
    const size_t size = *len;
    *qtable0 = nullptr;
    *qtable1 = nullptr;
    if (data[0] != 0xff || data[1] != 0xd8) return false;
    size_t position = 2;
    while (position + 4 <= size) {
        if (data[position++] != 0xff) return false;
        while (position < size && data[position] == 0xff) ++position;
        if (position + 3 > size) return false;
        const uint8_t marker = data[position++];
        const size_t blockSize = (size_t(data[position]) << 8) | data[position + 1];
        if (blockSize < 2 || blockSize > size - position) return false;
        const size_t end = position + blockSize;
        if (marker == 0xdb) {
            for (size_t table = position + 2; table < end;) {
                const uint8_t info = data[table++];
                if ((info >> 4) != 0 || (info & 15) > 1 || end - table < 64) return false;
                if ((info & 15) == 0) *qtable0 = data + table;
                else *qtable1 = data + table;
                table += 64;
            }
        } else if (marker == 0xda) {
            if (!*qtable0 || !*qtable1) return false;
            for (size_t scan = end; scan + 1 < size; ++scan) {
                if (data[scan] != 0xff) continue;
                const uint8_t following = data[++scan];
                if (following == 0x00) continue; // entropy byte stuffing
                if (following != 0xd9 || scan <= end + 1) return false;
                *start = data + end;
                *len = uint32_t(scan + 1 - end);
                return true;
            }
            return false;
        } else if (marker == 0xd8 || marker == 0xd9 || marker == 0x00) {
            return false;
        }
        position = end;
    }
    return false;
}

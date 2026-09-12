#pragma once

#include "platglue.h"
#include "LinkedListElement.h"

typedef unsigned const char *BufPtr;

class CRtspSession;

class CStreamer
{
public:
    static constexpr int kMaxSessions = 4;
    CStreamer( u_short width, u_short height );
    virtual ~CStreamer();

    CRtspSession *addSession( SOCKET aClient );
    LinkedListElement* getClientsListHead() { return &m_Clients; }

    int anySessions() { return m_Clients.NotEmpty(); }
    int sessionCount() const;
    int streamingSessionCount() const;

    bool handleRequests(uint32_t readTimeoutMs);

    u_short GetRtpServerPort();
    u_short GetRtcpServerPort();

    virtual void    streamImage(uint32_t curMsec) = 0; // send a new image to the client
    bool InitUdpTransport(void);
    void ReleaseUdpTransport(void);
    bool debug;
    void setNonBlockingTcpWrites(bool enabled) { m_nonBlockingTcpWrites = enabled; }
    void setServiceCallback(void (*callback)()) { m_serviceCallback = callback; }
    void setURI( String hostport, String pres = "mjpeg", String stream = "1" ); // set URI parts for sessions to use.
    String getURIHost(){ return m_URIHost; }; // for getting things back by sessions
    String getURIPresentation(){ return m_URIPresentation; };
    String getURIStream(){ return m_URIStream; };

protected:

    void    streamFrame(unsigned const char *data, uint32_t dataLen, uint32_t curMsec);

    String  m_URIHost; // Host:port URI part that client should use to connect. also it is reported in session answers where appropriate.
    String  m_URIPresentation; // name of presentation part of URI. sessions will check if client used correct one
    String  m_URIStream; // stream part of the URI.

private:
    int    SendRtpPacket(unsigned const char *jpeg, int jpegLen, int fragmentOffset, BufPtr quant0tbl = NULL, BufPtr quant1tbl = NULL);// returns new fragmentOffset or 0 if finished with frame

    UDPSOCKET m_RtpSocket;           // RTP socket for streaming RTP packets to client
    UDPSOCKET m_RtcpSocket;          // RTCP socket for sending/receiving RTCP packages

    IPPORT m_RtpServerPort;      // RTP sender port on server
    IPPORT m_RtcpServerPort;     // RTCP sender port on server

    u_short m_SequenceNumber;
    uint32_t m_Timestamp;
    int m_SendIdx;

    LinkedListElement m_Clients;
    uint32_t m_prevMsec;
    bool m_timestampInitialized;

    int m_udpRefCount;
    bool m_nonBlockingTcpWrites;
    void (*m_serviceCallback)();

    u_short m_width; // image data info
    u_short m_height;
};



// When JPEG is stored as a file it is wrapped in a container
// This function fixes up the provided start ptr to point to the
// actual JPEG stream data and returns the number of bytes skipped
// returns true if the file seems to be valid jpeg
// If quant tables can be found they will be stored in qtable0/1
bool decodeJPEGfile(BufPtr *start, uint32_t *len, BufPtr *qtable0, BufPtr *qtable1);

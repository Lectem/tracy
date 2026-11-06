#ifdef TRACY_WEBSOCKET_SUPPORT

#include <chrono>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/types.h>
#include <thread>

#include "TracyAssert.hpp"
#include "TracySocket.hpp"
#include "TracySystem.hpp"

#ifdef _WIN32
#  ifndef NOMINMAX
#    define NOMINMAX
#  endif
#  include <winsock2.h>
#  include <ws2tcpip.h>
#  ifdef _MSC_VER
#    pragma warning(disable:4244)
#    pragma warning(disable:4267)
#  endif
#  define poll WSAPoll
#else
#  include <arpa/inet.h>
#  include <sys/socket.h>
#  include <poll.h>
#endif

namespace tracy
{

#ifdef _WIN32
typedef SOCKET socket_t;
#else
typedef int socket_t;
#endif

static inline bool WsIEq( const char* a, const char* b, size_t n )
{
    for( size_t i=0; i<n; i++ )
    {
        char ca = a[i];
        char cb = b[i];
        if( ca >= 'A' && ca <= 'Z' ) ca += 'a' - 'A';
        if( cb >= 'A' && cb <= 'Z' ) cb += 'a' - 'A';
        if( ca != cb ) return false;
    }
    return true;
}

static bool WsParseIPv4( const char* s, size_t n, uint32_t& out )
{
    char buf[16];
    if( n >= sizeof( buf ) ) return false;
    memcpy( buf, s, n );
    buf[n] = '\0';
    in_addr addr;
    if( inet_pton( AF_INET, buf, &addr ) != 1 ) return false;
    out = ntohl( addr.s_addr );
    return true;
}

struct WsAuthority
{
    const char* scheme;
    size_t schemeLen;
    const char* host;
    size_t hostLen;
    int port;       // -1 when not specified
};

// [scheme://]host[:port], host being a name, an IPv4 address, a [bracketed] IPv6 address, or a pattern.
static bool WsParseAuthority( const char* s, size_t n, WsAuthority& a )
{
    a.scheme = nullptr;
    a.schemeLen = 0;
    a.port = -1;
    for( size_t i=0; i+2<n; i++ )
    {
        if( s[i] == ':' && s[i+1] == '/' && s[i+2] == '/' )
        {
            a.scheme = s;
            a.schemeLen = i;
            s += i + 3;
            n -= i + 3;
            break;
        }
    }
    if( n == 0 ) return false;

    size_t hostEnd = n;
    if( s[0] == '[' )
    {
        size_t i = 1;
        while( i < n && s[i] != ']' ) i++;
        if( i == n ) return false;
        hostEnd = i + 1;
    }
    else
    {
        for( size_t i=n; i>0; i-- )
        {
            if( s[i-1] == ':' )
            {
                hostEnd = i - 1;
                break;
            }
        }
    }
    if( hostEnd == 0 ) return false;

    if( hostEnd < n )
    {
        if( s[hostEnd] != ':' ) return false;
        const auto portStr = s + hostEnd + 1;
        const auto portLen = n - hostEnd - 1;
        if( portLen == 1 && portStr[0] == '*' )
        {
            a.port = -1;
        }
        else
        {
            if( portLen == 0 || portLen > 5 ) return false;
            int port = 0;
            for( size_t i=0; i<portLen; i++ )
            {
                if( portStr[i] < '0' || portStr[i] > '9' ) return false;
                port = port * 10 + ( portStr[i] - '0' );
            }
            if( port > 65535 ) return false;
            a.port = port;
        }
    }
    a.host = s;
    a.hostLen = hostEnd;
    return true;
}

static bool WsHostMatches( const WsAuthority& e, const WsAuthority& o )
{
    // CIDR range, matched against IPv4 literal origins only. Names are never resolved.
    for( size_t i=0; i<e.hostLen; i++ )
    {
        if( e.host[i] == '/' )
        {
            uint32_t net, ip;
            if( !WsParseIPv4( e.host, i, net ) ) return false;
            if( !WsParseIPv4( o.host, o.hostLen, ip ) ) return false;
            const auto bitsStr = e.host + i + 1;
            const auto bitsLen = e.hostLen - i - 1;
            if( bitsLen == 0 || bitsLen > 2 ) return false;
            uint32_t bits = 0;
            for( size_t j=0; j<bitsLen; j++ )
            {
                if( bitsStr[j] < '0' || bitsStr[j] > '9' ) return false;
                bits = bits * 10 + ( bitsStr[j] - '0' );
            }
            if( bits > 32 ) return false;
            const uint32_t mask = bits == 0 ? 0 : ~uint32_t( 0 ) << ( 32 - bits );
            return ( ip & mask ) == ( net & mask );
        }
    }
    if( e.hostLen == 1 && e.host[0] == '*' ) return true;
    if( e.hostLen > 2 && e.host[0] == '*' && e.host[1] == '.' )
    {
        const auto suffix = e.host + 1;
        const auto sl = e.hostLen - 1;
        return o.hostLen > sl && WsIEq( o.host + o.hostLen - sl, suffix, sl );
    }
    return o.hostLen == e.hostLen && WsIEq( o.host, e.host, e.hostLen );
}

bool WebSocketOriginAllowed( const char* origin, size_t len, const char* allowList )
{
    WsAuthority o;
    if( !WsParseAuthority( origin, len, o ) || !o.scheme ) return false;    // also rejects "null"
    const bool https = o.schemeLen == 5 && WsIEq( o.scheme, "https", 5 );
    const bool http = o.schemeLen == 4 && WsIEq( o.scheme, "http", 4 );
    if( !https && !http ) return false;
    const int port = o.port != -1 ? o.port : ( https ? 443 : 80 );

    // Loopback origins are always allowed.
    uint32_t ip;
    if( ( o.hostLen == 9 && WsIEq( o.host, "localhost", 9 ) ) ||
        ( o.hostLen == 5 && WsIEq( o.host, "[::1]", 5 ) ) ||
        ( WsParseIPv4( o.host, o.hostLen, ip ) && ( ip >> 24 ) == 127 ) )
    {
        return true;
    }

    if( !allowList ) return false;
    auto p = allowList;
    while( *p )
    {
        while( *p == ',' || *p == ';' || *p == ' ' || *p == '\t' || *p == '\r' || *p == '\n' ) p++;
        const auto start = p;
        while( *p && *p != ',' && *p != ';' && *p != ' ' && *p != '\t' && *p != '\r' && *p != '\n' ) p++;
        if( p == start ) continue;

        // A trailing slash is allowed, as in a URL.
        auto end = p;
        while( end - start > 1 && end[-1] == '/' ) end--;

        WsAuthority e;
        if( !WsParseAuthority( start, end - start, e ) ) continue;
        if( e.scheme && ( e.schemeLen != o.schemeLen || !WsIEq( e.scheme, o.scheme, e.schemeLen ) ) ) continue;
        if( e.port != -1 && e.port != port ) continue;
        if( WsHostMatches( e, o ) ) return true;
    }
    return false;
}

static void WsSha1( const uint8_t* data, size_t len, uint8_t out[20] )
{
    TRACY_ASSERT( len <= 119 );
    uint8_t msg[128] = {};
    memcpy( msg, data, len );
    msg[len] = 0x80;
    const size_t blocks = len + 9 <= 64 ? 1 : 2;
    const uint64_t bits = uint64_t( len ) * 8;
    for( int i=0; i<8; i++ ) msg[blocks*64 - 1 - i] = uint8_t( bits >> ( 8 * i ) );

    uint32_t h[5] = { 0x67452301, 0xEFCDAB89, 0x98BADCFE, 0x10325476, 0xC3D2E1F0 };
    for( size_t b=0; b<blocks; b++ )
    {
        uint32_t w[80];
        for( int i=0; i<16; i++ )
        {
            const auto m = msg + b*64 + i*4;
            w[i] = ( uint32_t( m[0] ) << 24 ) | ( uint32_t( m[1] ) << 16 ) | ( uint32_t( m[2] ) << 8 ) | m[3];
        }
        for( int i=16; i<80; i++ )
        {
            const uint32_t x = w[i-3] ^ w[i-8] ^ w[i-14] ^ w[i-16];
            w[i] = ( x << 1 ) | ( x >> 31 );
        }
        uint32_t a = h[0], bb = h[1], c = h[2], d = h[3], e = h[4];
        for( int i=0; i<80; i++ )
        {
            uint32_t f, k;
            if( i < 20 )      { f = ( bb & c ) | ( ~bb & d );            k = 0x5A827999; }
            else if( i < 40 ) { f = bb ^ c ^ d;                          k = 0x6ED9EBA1; }
            else if( i < 60 ) { f = ( bb & c ) | ( bb & d ) | ( c & d ); k = 0x8F1BBCDC; }
            else              { f = bb ^ c ^ d;                          k = 0xCA62C1D6; }
            const uint32_t t = ( ( a << 5 ) | ( a >> 27 ) ) + f + e + k + w[i];
            e = d;
            d = c;
            c = ( bb << 30 ) | ( bb >> 2 );
            bb = a;
            a = t;
        }
        h[0] += a; h[1] += bb; h[2] += c; h[3] += d; h[4] += e;
    }
    for( int i=0; i<5; i++ )
    {
        out[i*4+0] = uint8_t( h[i] >> 24 );
        out[i*4+1] = uint8_t( h[i] >> 16 );
        out[i*4+2] = uint8_t( h[i] >> 8 );
        out[i*4+3] = uint8_t( h[i] );
    }
}

// 20 bytes in, 28 characters + NUL out.
static void WsBase64Sha1( const uint8_t* in, char* out )
{
    static const char tbl[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    int o = 0;
    for( int i=0; i<18; i+=3 )
    {
        const uint32_t v = ( uint32_t( in[i] ) << 16 ) | ( uint32_t( in[i+1] ) << 8 ) | in[i+2];
        out[o++] = tbl[( v >> 18 ) & 63];
        out[o++] = tbl[( v >> 12 ) & 63];
        out[o++] = tbl[( v >> 6 ) & 63];
        out[o++] = tbl[v & 63];
    }
    const uint32_t v = ( uint32_t( in[18] ) << 16 ) | ( uint32_t( in[19] ) << 8 );
    out[o++] = tbl[( v >> 18 ) & 63];
    out[o++] = tbl[( v >> 12 ) & 63];
    out[o++] = tbl[( v >> 6 ) & 63];
    out[o++] = '=';
    out[o] = 0;
}

// Finds a header value in an HTTP request. Name is lowercase.
static bool WsFindHeader( const char* req, size_t reqLen, const char* name, const char*& val, size_t& valLen )
{
    const auto nameLen = strlen( name );
    const auto end = req + reqLen;
    auto p = (const char*)memchr( req, '\n', reqLen );     // skip the request line
    while( p && p + 1 < end )
    {
        p++;
        auto eol = (const char*)memchr( p, '\n', end - p );
        if( !eol ) break;
        auto lineEnd = eol;
        if( lineEnd > p && lineEnd[-1] == '\r' ) lineEnd--;
        if( lineEnd == p ) break;
        if( size_t( lineEnd - p ) > nameLen && p[nameLen] == ':' && WsIEq( p, name, nameLen ) )
        {
            auto v = p + nameLen + 1;
            while( v < lineEnd && ( *v == ' ' || *v == '\t' ) ) v++;
            auto ve = lineEnd;
            while( ve > v && ( ve[-1] == ' ' || ve[-1] == '\t' ) ) ve--;
            val = v;
            valLen = ve - v;
            return true;
        }
        p = eol;
    }
    return false;
}

// Case insensitive token search in a comma separated header value.
static bool WsHasToken( const char* val, size_t len, const char* token )
{
    const auto tl = strlen( token );
    size_t i = 0;
    while( i < len )
    {
        while( i < len && ( val[i] == ',' || val[i] == ' ' || val[i] == '\t' ) ) i++;
        const auto start = i;
        while( i < len && val[i] != ',' && val[i] != ' ' && val[i] != '\t' ) i++;
        if( i - start == tl && WsIEq( val + start, token, tl ) ) return true;
    }
    return false;
}

static int WsMakeHeader( uint8_t* h, uint64_t len )
{
    h[0] = 0x82;    // FIN, binary
    if( len < 126 )
    {
        h[1] = uint8_t( len );
        return 2;
    }
    if( len < 65536 )
    {
        h[1] = 126;
        h[2] = uint8_t( len >> 8 );
        h[3] = uint8_t( len );
        return 4;
    }
    h[1] = 127;
    for( int i=0; i<8; i++ ) h[2+i] = uint8_t( len >> ( 56 - 8 * i ) );
    return 10;
}

int Socket::WsDetectProtocol( int timeout )
{
    const auto sock = m_sock.load( std::memory_order_relaxed );
    struct pollfd fd;
    fd.fd = (socket_t)sock;
    fd.events = POLLIN;
    const auto end = std::chrono::steady_clock::now() + std::chrono::milliseconds( timeout );
    if( poll( &fd, 1, timeout ) <= 0 ) return -1;

    // The first bytes can arrive in pieces. Peeking doesn't consume them, so wait until all four are there.
    char p[4];
    for(;;)
    {
        const auto n = recv( sock, p, 4, MSG_PEEK );
        if( n == 0 ) return 0;
        if( n < 0 ) return -1;
        if( memcmp( p, "GET ", n ) != 0 )
        {
            m_wsState = WsRaw;
            return 1;
        }
        if( n == 4 ) break;
        if( std::chrono::steady_clock::now() >= end ) return -1;
        std::this_thread::sleep_for( std::chrono::milliseconds( 1 ) );
    }
    if( !WsHandshake( timeout ) ) return 0;
    m_wsState = WsActive;
    return 1;
}

bool Socket::WsHandshake( int timeout )
{
    const auto sock = m_sock.load( std::memory_order_relaxed );
    char req[16384];
    size_t len = 0;
    size_t hdrEnd = 0;
    const auto end = std::chrono::steady_clock::now() + std::chrono::milliseconds( timeout );
    while( !hdrEnd && len < sizeof( req ) )
    {
        auto left = std::chrono::duration_cast<std::chrono::milliseconds>( end - std::chrono::steady_clock::now() ).count();
        if( left < 0 ) left = 0;
        struct pollfd fd;
        fd.fd = (socket_t)sock;
        fd.events = POLLIN;
        const auto pr = poll( &fd, 1, (int)left );
        if( pr == 0 ) break;
        if( pr < 0 )
        {
            if( left == 0 ) break;
            continue;
        }
        // The client waits for the response before it sends frames, so only the request is read here.
        const auto r = recv( sock, req + len, sizeof( req ) - len, 0 );
        if( r <= 0 ) return false;
        const auto prev = len;
        len += r;
        for( size_t j = prev > 3 ? prev - 3 : 0; j+3<len; j++ )
        {
            if( memcmp( req + j, "\r\n\r\n", 4 ) == 0 )
            {
                hdrEnd = j + 4;
                break;
            }
        }
    }

    static const char bad[] = "HTTP/1.1 400 Bad Request\r\nContent-Length: 0\r\nConnection: close\r\n\r\n";
    static const char tooLarge[] = "HTTP/1.1 431 Request Header Fields Too Large\r\nContent-Length: 0\r\nConnection: close\r\n\r\n";
    static const char forbidden[] = "HTTP/1.1 403 Forbidden\r\nContent-Length: 0\r\nConnection: close\r\n\r\n";
    if( !hdrEnd )
    {
        if( len == sizeof( req ) ) SendRaw( tooLarge, sizeof( tooLarge ) - 1 );
        else SendRaw( bad, sizeof( bad ) - 1 );
        return false;
    }

    const char* val;
    size_t valLen;
    if( !WsFindHeader( req, hdrEnd, "upgrade", val, valLen ) || !WsHasToken( val, valLen, "websocket" ) ||
        !WsFindHeader( req, hdrEnd, "connection", val, valLen ) || !WsHasToken( val, valLen, "upgrade" ) ||
        !WsFindHeader( req, hdrEnd, "sec-websocket-version", val, valLen ) || valLen != 2 || memcmp( val, "13", 2 ) != 0 )
    {
        SendRaw( bad, sizeof( bad ) - 1 );
        return false;
    }

    if( WsFindHeader( req, hdrEnd, "origin", val, valLen ) )
    {
#ifdef TRACY_WEBSOCKET_ORIGINS
        const char* def = TRACY_WEBSOCKET_ORIGINS;
#else
        const char* def = "";
#endif
        const char* env = GetEnvVar( "TRACY_WEBSOCKET_ORIGINS" );
        if( !WebSocketOriginAllowed( val, valLen, def ) && !( env && WebSocketOriginAllowed( val, valLen, env ) ) )
        {
            SendRaw( forbidden, sizeof( forbidden ) - 1 );
            return false;
        }
    }

    if( !WsFindHeader( req, hdrEnd, "sec-websocket-key", val, valLen ) || valLen != 24 )
    {
        SendRaw( bad, sizeof( bad ) - 1 );
        return false;
    }
    uint8_t keyGuid[60];
    memcpy( keyGuid, val, 24 );
    memcpy( keyGuid + 24, "258EAFA5-E914-47DA-95CA-C5AB0DC85B11", 36 );
    uint8_t digest[20];
    WsSha1( keyGuid, sizeof( keyGuid ), digest );
    char accept[32];
    WsBase64Sha1( digest, accept );

    bool binary = false;
    if( WsFindHeader( req, hdrEnd, "sec-websocket-protocol", val, valLen ) ) binary = WsHasToken( val, valLen, "binary" );

    char resp[256];
    const auto rl = snprintf( resp, sizeof( resp ),
        "HTTP/1.1 101 Switching Protocols\r\nUpgrade: websocket\r\nConnection: Upgrade\r\nSec-WebSocket-Accept: %s\r\n%s\r\n",
        accept, binary ? "Sec-WebSocket-Protocol: binary\r\n" : "" );
    return SendRaw( resp, rl ) != -1;
}

int Socket::WsRecv( void* _buf, int len, int timeout )
{
    auto& f = m_wsFrame;
    auto buf = (uint8_t*)_buf;
    for(;;)
    {
        if( f.Pending() )
        {
            const int n = f.remain < uint64_t( len ) ? int( f.remain ) : len;
            const auto r = RecvRaw( buf, n, timeout );
            if( r <= 0 ) return r;
            for( int i=0; i<r; i++ ) buf[i] ^= f.mask[( f.maskOff + i ) & 3];
            f.maskOff = uint8_t( f.maskOff + r );
            f.remain -= r;
            if( f.remain == 0 ) f.hdrDone = false;
            return r;
        }
        const auto r = WsFrameStep( timeout );
        if( r <= 0 ) return r;
    }
}

// Reads a frame header, and the whole frame if it is a control frame.
// Returns 1 on progress, 0 when the connection is closed or broken, -1 when no data is available yet.
int Socket::WsFrameStep( int timeout )
{
    auto& f = m_wsFrame;
    if( !f.hdrDone )
    {
        for(;;)
        {
            int need = 2;
            if( f.hdrLen >= 2 )
            {
                const auto l7 = f.hdr[1] & 0x7f;
                need += ( l7 == 126 ? 2 : l7 == 127 ? 8 : 0 ) + ( ( f.hdr[1] & 0x80 ) ? 4 : 0 );
            }
            if( f.hdrLen >= need ) break;
            const auto r = RecvRaw( f.hdr + f.hdrLen, need - f.hdrLen, timeout );
            if( r <= 0 ) return r;
            f.hdrLen += r;
        }

        if( !( f.hdr[1] & 0x80 ) ) return 0;      // client frames must be masked
        f.opcode = f.hdr[0] & 0x0f;
        const auto l7 = f.hdr[1] & 0x7f;
        uint64_t plen;
        int off = 2;
        if( l7 == 126 )
        {
            plen = ( uint64_t( f.hdr[2] ) << 8 ) | f.hdr[3];
            off = 4;
        }
        else if( l7 == 127 )
        {
            plen = 0;
            for( int i=0; i<8; i++ ) plen = ( plen << 8 ) | f.hdr[2+i];
            off = 10;
        }
        else
        {
            plen = l7;
        }
        memcpy( f.mask, f.hdr + off, 4 );
        f.maskOff = 0;
        f.ctlLen = 0;
        f.remain = plen;
        const bool fin = ( f.hdr[0] & 0x80 ) != 0;
        f.hdrLen = 0;

        if( f.opcode >= 8 )
        {
            if( plen > 125 || !fin ) return 0;
        }
        else if( f.opcode > 2 )
        {
            return 0;
        }
        f.hdrDone = true;
        if( f.opcode < 8 )
        {
            if( plen == 0 ) f.hdrDone = false;
            return 1;
        }
    }

    // Control frame payload.
    while( f.remain > 0 )
    {
        const auto r = RecvRaw( f.ctl + f.ctlLen, int( f.remain ), timeout );
        if( r <= 0 ) return r;
        for( int i=0; i<r; i++ ) f.ctl[f.ctlLen+i] ^= f.mask[( f.maskOff + i ) & 3];
        f.maskOff = uint8_t( f.maskOff + r );
        f.ctlLen = uint8_t( f.ctlLen + r );
        f.remain -= r;
    }
    f.hdrDone = false;
    switch( f.opcode )
    {
    case 8:     // close
        WsSendControl( 8, f.ctl, f.ctlLen >= 2 ? 2 : 0 );
        return 0;
    case 9:     // ping
        WsSendControl( 10, f.ctl, f.ctlLen );
        return 1;
    case 10:    // pong
        return 1;
    default:
        return 0;
    }
}

bool Socket::WsHasData()
{
    // A broken connection counts as having data, as with a plain socket, so that the next read reports it.
    struct pollfd fd;
    fd.fd = (socket_t)m_sock.load( std::memory_order_relaxed );
    fd.events = POLLIN;
    if( poll( &fd, 1, 0 ) > 0 && ( fd.revents & ( POLLERR | POLLHUP ) ) ) return true;

    // Consume control frames, report only when payload data is pending (or the connection is gone).
    // The number of frames is limited, or a peer sending nothing else would keep the caller from checking if it
    // should exit. The rest is read on the next call.
    for( int i=0; i<16; i++ )
    {
        if( m_wsFrame.Pending() ) return true;
        const auto r = WsFrameStep( 0 );
        if( r == 0 ) return true;
        if( r < 0 ) return false;
    }
    return m_wsFrame.Pending();
}

// Called from the receive path. Only the profiler thread uses the socket, so this can't land between the parts of a send.
void Socket::WsSendControl( uint8_t opcode, const uint8_t* data, int len )
{
    uint8_t frame[2+125];
    frame[0] = 0x80 | opcode;
    frame[1] = uint8_t( len );
    memcpy( frame + 2, data, len );
    SendRaw( frame, 2 + len );
}

int Socket::WsSend( const void* buf, int len )
{
    uint8_t hdr[WebSocketMaxHeaderSize];
    const auto hl = WsMakeHeader( hdr, len );
    if( len <= 1024 )
    {
        uint8_t tmp[WebSocketMaxHeaderSize + 1024];
        memcpy( tmp, hdr, hl );
        memcpy( tmp + hl, buf, len );
        return SendRaw( tmp, hl + len ) == -1 ? -1 : len;
    }
    if( SendRaw( hdr, hl ) == -1 ) return -1;
    return SendRaw( buf, len );
}

}

#endif

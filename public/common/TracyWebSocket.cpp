#ifdef TRACY_WEBSOCKET_SUPPORT

#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/types.h>

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

}

#endif

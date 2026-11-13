// Captures an in-process callstack with tracy::Callstack() from inside work(), called with a temporary std::string
// whose destructor is inlined in caller() right after the call. Prints, for each stored frame:
//   FRAME <i> <module-relative offset hex> <inline chain, innermost first, '|' separated>
// The test script compares the chain with addr2line -i.
#include <dlfcn.h>
#include <stdio.h>
#include <string>
#include "tracy/Tracy.hpp"
#include "client/TracyCallstack.hpp"

static void dump()
{
    auto* cs = (uintptr_t*)tracy::Callstack( 12 );
    const auto n = cs[0];
    for( uintptr_t i=1; i<=n; i++ )
    {
        Dl_info di;
        if( !dladdr( (void*)cs[i], &di ) ) continue;
        auto e = tracy::DecodeCallstackPtr( cs[i] );
        std::string chain;
        for( uint8_t j=0; j<e.size; j++ ) { if( j ) chain += '|'; chain += e.data[j].name; }
        printf( "FRAME %zu %s %lx %s\n", (size_t)i-1, di.dli_fname, (unsigned long)( cs[i] - (uintptr_t)di.dli_fbase ), chain.c_str() );
    }
    tracy::tracy_free( cs );
}

__attribute__((noinline)) long work( const std::string& s )
{
    dump();
    return (long)s.size();
}

__attribute__((noinline)) void caller( int n )
{
    work( std::string( "a string long enough to need heap storage, not the small string buffer" ) + std::to_string( n ) );
}

int main()
{
    ZoneScoped;
    caller( 1 );
    return 0;
}

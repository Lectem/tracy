// Target of run-etw.bat: a hot leaf (tiny), a never called function right after it (neighbor) and a caller with a temporary.
#include "tracy/Tracy.hpp"
#include <string>
#include <thread>
#include <windows.h>
__declspec(noinline) long neighbor( long x ) { return x * 7 + 3; }     // never called: must never be a sampled leaf
__declspec(noinline) long tiny( long x ) { return x * 3 + 1; }         // 1-2 instructions: samples land on its first one
__declspec(noinline) long work( const std::string& s ) { volatile long x = 0; for( long i=0; i<300000000; i++ ) x += (long)s.size(); return x; }
__declspec(noinline) void caller( int n ) { work( std::string( "a string long enough to need heap storage, not the small string buffer" ) + std::to_string( n ) ); }
// A thread that blocks: its wait stacks are user frames (sleeper, SleepEx, NtDelayExecution) above kernel frames.
// sleeper_next is never called: if the return address of the call to Sleep is not moved into the call, it may resolve there.
__declspec(noinline) void sleeper() { for(;;) Sleep( 1 ); }
__declspec(noinline) void sleeper_next() { Sleep( 2 ); }

int main( int argc, char** )
{
    std::thread( sleeper ).detach();
    if( argc > 99 ) sleeper_next();
    volatile long acc = 0;
    for( int i=0; ; i++ )
    {
        caller( i );
        for( int k=0; k<300000000; k++ ) acc = tiny( acc + k );
        if( argc > 99 ) neighbor( 1 );
    }
}

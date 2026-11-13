// tracy-stackdump: print the sampled callstacks of a trace as folded stacks ("outer;...;inner count"), most frequent first.
// -c dumps the callstacks of the context switch samples (thread blocked) instead of the periodic samples. Inline frames are expanded. With -a, every physical frame is prefixed with its stored address: "0xADDR:name".
// Test/diagnostic tool for the callstack address contract; not part of the shipped tools.
#include <algorithm>
#include <map>
#include <memory>
#include <string>
#include <vector>
#include <stdio.h>
#include <string.h>

#include "../../../../server/TracyFileRead.hpp"
#include "../../../../server/TracyWorker.hpp"

int main( int argc, char** argv )
{
    bool addr = false, ctx = false;
    const char* file = nullptr;
    size_t top = 20;
    for( int i=1; i<argc; i++ )
    {
        if( !strcmp( argv[i], "-a" ) ) addr = true;
        else if( !strcmp( argv[i], "-c" ) ) ctx = true;
        else if( !strcmp( argv[i], "-n" ) && i+1<argc ) top = strtoul( argv[++i], nullptr, 10 );
        else file = argv[i];
    }
    if( !file ) { fprintf( stderr, "usage: tracy-stackdump [-a] [-c] [-n N] trace.tracy\n" ); return 1; }
    auto f = std::unique_ptr<tracy::FileRead>( tracy::FileRead::Open( file ) );
    if( !f ) { fprintf( stderr, "Could not open %s\n", file ); return 1; }
    auto worker = tracy::Worker( *f );

    std::map<std::string, uint64_t> folded;
    for( auto& td : worker.GetThreadData() )
    {
        auto collect = [&]( auto& vec )
        {
            for( auto& s : vec )
            {
                const auto& cs = worker.GetCallstack( s.callstack.Val() );
                std::string str;
                for( int i = int( cs.size() ) - 1; i >= 0; i-- )
                {
                    auto fd = worker.GetCallstackFrame( cs[i] );
                    if( !fd ) { str += ( str.empty() ? "" : ";" ); str += "[nodata]"; continue; }
                    for( int j = int( fd->size ) - 1; j >= 0; j-- )
                    {
                        if( !str.empty() ) str += ';';
                        if( addr && j == 0 )
                        {
                            char b[32]; snprintf( b, sizeof b, "0x%llx:", (unsigned long long)worker.GetCanonicalPointer( cs[i] ) ); str += b;
                        }
                        str += worker.GetString( fd->data[j].name );
                    }
                }
                folded[str]++;
            }
        };
        if( ctx ) collect( td->ctxSwitchSamples ); else collect( td->samples );
    }
    std::vector<std::pair<std::string, uint64_t>> v( folded.begin(), folded.end() );
    std::sort( v.begin(), v.end(), []( auto& a, auto& b ) { return a.second > b.second; } );
    for( size_t i=0; i<v.size() && i<top; i++ ) printf( "%s %llu\n", v[i].first.c_str(), (unsigned long long)v[i].second );
    return 0;
}

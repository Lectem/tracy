#ifndef __TRACYFILESYSTEM_HPP__
#define __TRACYFILESYSTEM_HPP__

#include <stdint.h>
#include <sys/stat.h>

namespace tracy
{

class View;
class Worker;

// Size of a regular file, -1 if there is none.
static inline int64_t FileSize( const char* fn )
{
    struct stat buf;
    if( stat( fn, &buf ) != 0 || ( buf.st_mode & S_IFREG ) == 0 ) return -1;
    return buf.st_size;
}

static inline bool FileExists( const char* fn ) { return FileSize( fn ) >= 0; }

bool SourceFileValid( const char* fn, uint64_t olderThan, const View& view, const Worker& worker );

}

#endif

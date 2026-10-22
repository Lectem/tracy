#include <algorithm>
#include <chrono>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>

#include "TracyAttach.hpp"

#ifdef __linux__
#  include <dirent.h>
#  include <errno.h>
#  include <fcntl.h>
#  include <limits.h>
#  include <poll.h>
#  include <signal.h>
#  include <spawn.h>
#  include <sys/types.h>
#  include <sys/wait.h>
#  include <unistd.h>
extern char** environ;
#endif

namespace tracy
{

// Bounds for a target that prints without end.
constexpr size_t MaxLogLines = 2000;
constexpr size_t MaxReadPerPoll = 256 * 1024;
constexpr size_t MaxLineLength = 64 * 1024;

[[maybe_unused]] static int64_t NowMs()
{
    return std::chrono::duration_cast<std::chrono::milliseconds>( std::chrono::steady_clock::now().time_since_epoch() ).count();
}

static std::string Trim( const std::string& s )
{
    const auto b = s.find_first_not_of( " \t\r" );
    if( b == std::string::npos ) return {};
    const auto e = s.find_last_not_of( " \t\r" );
    return s.substr( b, e - b + 1 );
}

std::string DefaultMonitorPath()
{
    if( const char* env = getenv( "TRACY_MONITOR" ) )
    {
        if( *env ) return env;
    }
#ifdef __linux__
    char exe[PATH_MAX];
    const ssize_t n = readlink( "/proc/self/exe", exe, sizeof( exe ) - 1 );
    if( n > 0 )
    {
        exe[n] = '\0';
        if( char* slash = strrchr( exe, '/' ) )
        {
            *slash = '\0';
            std::string candidate = std::string( exe ) + "/tracy-monitor";
            if( access( candidate.c_str(), X_OK ) == 0 ) return candidate;
        }
    }
#endif
    return "tracy-monitor";
}

bool MonitorAvailable()
{
#ifdef __linux__
    const std::string path = DefaultMonitorPath();
    if( path.find( '/' ) != std::string::npos ) return access( path.c_str(), X_OK ) == 0;
    const char* env = getenv( "PATH" );
    if( !env ) return false;
    std::string dirs = env;
    size_t pos = 0;
    while( pos <= dirs.size() )
    {
        auto end = dirs.find( ':', pos );
        if( end == std::string::npos ) end = dirs.size();
        const std::string dir = end == pos ? "." : dirs.substr( pos, end - pos );
        if( access( ( dir + "/" + path ).c_str(), X_OK ) == 0 ) return true;
        pos = end + 1;
    }
    return false;
#else
    return true;
#endif
}

#ifdef __linux__

std::vector<AttachTarget> ListAttachTargets()
{
    std::vector<AttachTarget> targets;
    DIR* proc = opendir( "/proc" );
    if( !proc ) return targets;

    const uint32_t self = (uint32_t)getuid();
    struct dirent* ent;
    while( ( ent = readdir( proc ) ) )
    {
        if( ent->d_name[0] < '0' || ent->d_name[0] > '9' ) continue;
        const int pid = atoi( ent->d_name );

        char path[64];
        std::string comm;
        std::string cmdline;
        std::string status;

        snprintf( path, sizeof( path ), "/proc/%d/comm", pid );
        if( FILE* f = fopen( path, "r" ) )
        {
            char buf[64] = {};
            if( fgets( buf, sizeof( buf ), f ) ) comm = Trim( buf );
            fclose( f );
        }

        snprintf( path, sizeof( path ), "/proc/%d/cmdline", pid );
        if( FILE* f = fopen( path, "r" ) )
        {
            char buf[4096];
            size_t n;
            while( ( n = fread( buf, 1, sizeof( buf ), f ) ) > 0 )
            {
                for( size_t i=0; i < n; i++ ) cmdline += buf[i] == '\0' ? ' ' : buf[i];
            }
            fclose( f );
        }
        cmdline = Trim( cmdline );

        // Kernel threads have no cmdline.
        if( comm.empty() || cmdline.empty() ) continue;

        uint32_t uid = UINT32_MAX;
        snprintf( path, sizeof( path ), "/proc/%d/status", pid );
        if( FILE* f = fopen( path, "r" ) )
        {
            char line[256];
            while( fgets( line, sizeof( line ), f ) )
            {
                unsigned int realUid;
                if( sscanf( line, "Uid:\t%u", &realUid ) == 1 )
                {
                    uid = realUid;
                    break;
                }
            }
            fclose( f );
        }

        std::string exe;
        char exePath[PATH_MAX];
        snprintf( path, sizeof( path ), "/proc/%d/exe", pid );
        const ssize_t exeLen = readlink( path, exePath, sizeof( exePath ) - 1 );
        if( exeLen > 0 ) exe.assign( exePath, (size_t)exeLen );

        targets.push_back( AttachTarget { pid, uid, uid == self, comm, cmdline, exe } );
    }
    closedir( proc );

    std::sort( targets.begin(), targets.end(), []( const AttachTarget& a, const AttachTarget& b ) {
        if( a.name != b.name ) return a.name < b.name;
        return a.pid < b.pid;
    } );
    return targets;
}

#else

std::vector<AttachTarget> ListAttachTargets()
{
    return {};
}

#endif

AttachSession::AttachSession() {}

AttachSession::~AttachSession()
{
    if( m_pid >= 0 )
    {
#ifdef __linux__
        // SIGINT first, SIGKILL if the monitor does not exit in time.
        kill( m_pid, SIGINT );
        bool exited = false;
        for( int i=0; i < 100 && !exited; i++ )
        {
            exited = waitpid( m_pid, nullptr, WNOHANG ) == m_pid;
            if( !exited ) usleep( 10000 );
        }
        if( !exited )
        {
            kill( m_pid, SIGKILL );
            waitpid( m_pid, nullptr, 0 );
        }
#endif
        m_pid = -1;
    }
#ifdef __linux__
    if( m_outFd >= 0 ) close( m_outFd );
    if( m_errFd >= 0 ) close( m_errFd );
#endif
    m_outFd = m_errFd = -1;
}

bool AttachSession::Start( const std::string& monitorPath, const std::vector<std::string>& args, std::string& errorOut )
{
    if( IsRunning() )
    {
        errorOut = "a monitor is already running";
        return false;
    }
    Reset();

#ifndef __linux__
    errorOut = "attaching to a process is only available on Linux for now";
    return false;
#else
    int outPipe[2];
    int errPipe[2];
    if( pipe2( outPipe, O_CLOEXEC ) != 0 )
    {
        errorOut = strerror( errno );
        return false;
    }
    if( pipe2( errPipe, O_CLOEXEC ) != 0 )
    {
        errorOut = strerror( errno );
        close( outPipe[0] );
        close( outPipe[1] );
        return false;
    }

    // Built before spawning, the child only execs.
    std::vector<char*> argv;
    argv.push_back( const_cast<char*>( monitorPath.c_str() ) );
    for( const auto& a : args ) argv.push_back( const_cast<char*>( a.c_str() ) );
    argv.push_back( nullptr );

    posix_spawn_file_actions_t actions;
    posix_spawn_file_actions_init( &actions );
    posix_spawn_file_actions_addopen( &actions, 0, "/dev/null", O_RDONLY, 0 );
    posix_spawn_file_actions_adddup2( &actions, outPipe[1], 1 );
    posix_spawn_file_actions_adddup2( &actions, errPipe[1], 2 );

    pid_t pid = 0;
    const int rc = posix_spawnp( &pid, monitorPath.c_str(), &actions, nullptr, argv.data(), environ );
    posix_spawn_file_actions_destroy( &actions );

    close( outPipe[1] );
    close( errPipe[1] );
    if( rc != 0 )
    {
        errorOut = rc == ENOENT ? std::string( "tracy-monitor not found: put it next to the profiler or in PATH" ) : std::string( "cannot start tracy-monitor: " ) + strerror( rc );
        close( outPipe[0] );
        close( errPipe[0] );
        return false;
    }

    fcntl( outPipe[0], F_SETFL, O_NONBLOCK );
    fcntl( errPipe[0], F_SETFL, O_NONBLOCK );

    m_pid = (int)pid;
    m_outFd = outPipe[0];
    m_errFd = errPipe[0];
    m_state = AttachState::Starting;
    m_log.push_back( { false, "$ " + monitorPath } );
    for( const auto& a : args ) m_log.back().text += " " + a;
    return true;
#endif
}

void AttachSession::Stop()
{
    if( m_pid < 0 || m_stopRequested ) return;
#ifdef __linux__
    m_stopRequested = true;
    m_stopTimeMs = NowMs();
    kill( m_pid, SIGINT );  // saves nothing without -o
#endif
}

void AttachSession::Reset()
{
    if( IsRunning() ) return;
#ifdef __linux__
    // A target can keep the pipes open after the monitor exits.
    if( m_outFd >= 0 ) close( m_outFd );
    if( m_errFd >= 0 ) close( m_errFd );
#endif
    m_outFd = m_errFd = -1;
    m_state = AttachState::Idle;
    m_port = 0;
    m_exitCode = 0;
    m_stopRequested = false;
    m_onDemand = false;
    m_reportDone = false;
    m_target.clear();
    m_partialOut.clear();
    m_partialErr.clear();
    m_log.clear();
    m_sources.clear();
}

void AttachSession::MarkConnected()
{
    if( m_state == AttachState::Listening ) m_state = AttachState::Connected;
}

void AttachSession::Reconnect()
{
    if( m_state == AttachState::Connected ) m_state = AttachState::Listening;
}

void AttachSession::HandleLine( const std::string& line, bool isError )
{
    if( m_log.size() >= MaxLogLines ) m_log.erase( m_log.begin(), m_log.begin() + MaxLogLines / 4 );
    m_log.push_back( { isError, line } );
    if( isError ) return;

    if( m_state == AttachState::Starting )
    {
        // "Profiling 'name' (pid N) on port P"
        if( line.compare( 0, 11, "Profiling '" ) == 0 )
        {
            const auto end = line.find( '\'', 11 );
            if( end != std::string::npos ) m_target = line.substr( 11, end - 11 );
        }
        const auto p = line.find( "on port " );
        if( p != std::string::npos && line.compare( 0, 9, "Profiling" ) == 0 )
        {
            m_port = (uint16_t)atoi( line.c_str() + p + 8 );
            m_state = AttachState::Listening;
        }
    }
    else if( m_state == AttachState::Listening || m_state == AttachState::Connected )
    {
        // Source lines are indented. The first other line ends the report, as a launched target shares the pipe.
        if( line.size() <= 2 || line[0] != ' ' || line[1] != ' ' ) m_reportDone = true;
        else if( !m_reportDone )
        {
            const auto colon = line.find( ':' );
            if( colon != std::string::npos )
            {
                const std::string value = Trim( line.substr( colon + 1 ) );
                const auto space = value.find( ' ' );
                AttachSource src;
                src.name = Trim( line.substr( 0, colon ) );
                src.state = value.substr( 0, space );
                if( space != std::string::npos ) src.detail = Trim( value.substr( space + 1 ) );
                if( src.detail.size() >= 2 && src.detail.front() == '(' && src.detail.back() == ')' ) src.detail = src.detail.substr( 1, src.detail.size() - 2 );
                if( src.name == "recording" )
                {
                    m_onDemand = src.state == "on-demand";
                }
                else if( src.state == "yes" || src.state == "no" || src.state == "warning" )
                {
                    m_sources.push_back( std::move( src ) );
                }
            }
        }
    }
}

void AttachSession::ReadPipe( int& fd, bool isError )
{
#ifdef __linux__
    if( fd < 0 ) return;
    std::string& partial = isError ? m_partialErr : m_partialOut;
    char buf[4096];
    size_t total = 0;
    while( total < MaxReadPerPoll )
    {
        const ssize_t n = read( fd, buf, sizeof( buf ) );
        if( n > 0 )
        {
            total += (size_t)n;
            partial.append( buf, (size_t)n );
            // No newline for a long time, flush what we have.
            if( partial.size() > MaxLineLength ) partial.push_back( '\n' );
            size_t nl;
            while( ( nl = partial.find( '\n' ) ) != std::string::npos )
            {
                HandleLine( partial.substr( 0, nl ), isError );
                partial.erase( 0, nl + 1 );
            }
            continue;
        }
        if( n < 0 && errno == EINTR ) continue;
        if( n < 0 && errno == EAGAIN ) break;
        // EOF or error.
        if( !partial.empty() )
        {
            HandleLine( partial, isError );
            partial.clear();
        }
        close( fd );
        fd = -1;
        break;
    }
#endif
}

void AttachSession::Finish( int status )
{
#ifdef __linux__
    if( WIFEXITED( status ) )
    {
        m_exitCode = WEXITSTATUS( status );
    }
    else
    {
        m_exitCode = 128 + WTERMSIG( status );
    }
    m_pid = -1;
    m_state = ( m_exitCode == 0 || m_stopRequested ) ? AttachState::Stopped : AttachState::Failed;
    if( m_state == AttachState::Failed )
    {
        m_log.push_back( { true, "tracy-monitor exited with status " + std::to_string( m_exitCode ) } );
    }
#endif
}

void AttachSession::Poll()
{
#ifdef __linux__
    if( m_pid < 0 && m_outFd < 0 && m_errFd < 0 ) return;

    ReadPipe( m_outFd, false );
    ReadPipe( m_errFd, true );

    if( m_pid < 0 ) return;

    int status = 0;
    const pid_t waited = waitpid( m_pid, &status, WNOHANG );
    // -1 means the child was reaped elsewhere.
    if( waited == m_pid || ( waited < 0 && errno != EINTR ) )
    {
        ReadPipe( m_outFd, false );
        ReadPipe( m_errFd, true );
        Finish( waited == m_pid ? status : 0 );
        return;
    }

    // The exit ends the session, not EOF, as the target may hold the pipes.
    if( m_stopRequested && NowMs() - m_stopTimeMs > 3000 )
    {
        kill( m_pid, SIGKILL );
    }
#endif
}

}

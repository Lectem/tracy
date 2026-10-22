#ifndef __TRACYATTACH_HPP__
#define __TRACYATTACH_HPP__

#include <stdint.h>
#include <string>
#include <utility>
#include <vector>

namespace tracy
{

struct AttachTarget
{
    int pid;
    uint32_t uid;
    bool sameUser;
    std::string name;
    std::string cmdline;
    std::string exe;  // empty if /proc/<pid>/exe cannot be read
};

// Processes tracy-monitor can attach to. Empty on platforms without a backend.
std::vector<AttachTarget> ListAttachTargets();

// $TRACY_MONITOR, then tracy-monitor next to the profiler, then tracy-monitor from PATH.
std::string DefaultMonitorPath();

// Whether DefaultMonitorPath() names an executable that can be started.
bool MonitorAvailable();

enum class AttachState { Idle, Starting, Listening, Connected, Stopped, Failed };

struct AttachLogLine
{
    bool isError;
    std::string text;
};

// One line of the startup report: "  name: state detail". State is yes, no or warning.
struct AttachSource
{
    std::string name;
    std::string state;
    std::string detail;
};

// One tracy-monitor run. Only used from the UI thread.
class AttachSession
{
public:
    AttachSession();
    ~AttachSession();

    bool Start( const std::string& monitorPath, const std::vector<std::string>& args, std::string& errorOut );
    void Stop();
    void Reset();
    void Poll();
    void MarkConnected();
    void Reconnect();

    bool IsRunning() const { return m_pid >= 0; }
    AttachState GetState() const { return m_state; }
    uint16_t GetPort() const { return m_port; }
    int GetExitCode() const { return m_exitCode; }
    const std::string& GetTargetName() const { return m_target; }
    const std::vector<AttachLogLine>& GetLog() const { return m_log; }
    const std::vector<AttachSource>& GetSources() const { return m_sources; }
    bool IsOnDemand() const { return m_onDemand; }

private:
    void ReadPipe( int& fd, bool isError );
    void HandleLine( const std::string& line, bool isError );
    void Finish( int status );

    int m_pid = -1;
    int m_outFd = -1;
    int m_errFd = -1;
    bool m_stopRequested = false;
    bool m_onDemand = false;
    bool m_reportDone = false;  // the rest of the output is the target's
    int64_t m_stopTimeMs = 0;
    AttachState m_state = AttachState::Idle;
    uint16_t m_port = 0;
    int m_exitCode = 0;
    std::string m_target;
    std::string m_partialOut;
    std::string m_partialErr;
    std::vector<AttachLogLine> m_log;
    std::vector<AttachSource> m_sources;
};

}

#endif

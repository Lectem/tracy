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
    std::string exe;  // empty when /proc/<pid>/exe is not readable
};

// Processes tracy-monitor could try to attach to. Empty on platforms without a
// backend yet.
std::vector<AttachTarget> ListAttachTargets();

// $TRACY_MONITOR if set, then tracy-monitor next to the profiler executable,
// then tracy-monitor from PATH.
std::string DefaultMonitorPath();

// Whether DefaultMonitorPath() names an executable that can be started.
bool MonitorAvailable();

enum class AttachState { Idle, Starting, Listening, Connected, Stopped, Failed };

struct AttachLogLine
{
    bool isError;
    std::string text;
};

// One line of the monitor's startup report: "  name: state detail", where state is
// "yes", "no" or "warning". The detail is optional.
struct AttachSource
{
    std::string name;
    std::string state;
    std::string detail;
};

// One tracy-monitor run. Owned and polled by the UI thread, so no locking.
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
    // The monitor keeps listening after a viewer disconnects: Poll() hands its port out again.
    void Reconnect();

    bool IsRunning() const { return m_pid >= 0; }
    AttachState GetState() const { return m_state; }
    uint16_t GetPort() const { return m_port; }
    int GetExitCode() const { return m_exitCode; }
    const std::string& GetTargetName() const { return m_target; }
    const std::vector<AttachLogLine>& GetLog() const { return m_log; }
    // Startup report of tracy-monitor: (source, value), value is "yes" or "no (reason)".
    const std::vector<AttachSource>& GetSources() const { return m_sources; }
    // Whether the monitor was built with on-demand recording, so it accepts a new viewer after the first.
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

#ifndef __HISTORY_HPP__
#define __HISTORY_HPP__

#include <stddef.h>
#include <stdint.h>
#include <string>
#include <unordered_map>
#include <vector>

struct AttachHistoryEntry
{
    int pid;
    std::string exe;
    std::string cmdline;
};

// Persisted "recently used" lists, stored together in a single history.json
// in the profiler's save directory.
class History
{
public:
    History();
    ~History();

    // Client addresses, most used first.
    size_t ConnectionCount() const { return m_connectionsVec.size(); }
    const std::string& ConnectionName( size_t idx ) const { return m_connectionsVec[idx]->first; }
    void CountConnection( const std::string& address );
    void EraseConnection( size_t idx );

    // Trace files that were opened or saved, most recent first.
    const std::vector<std::string>& RecentTraces() const { return m_recentTraces; }
    void RecordTrace( const char* path );

    // Attached processes, most recent first.
    const std::vector<AttachHistoryEntry>& Attached() const { return m_attached; }
    void RecordAttach( const AttachHistoryEntry& entry );

    // Text of the process filter of the Monitor tab.
    const std::string& AttachFilter() const { return m_attachFilter; }
    void SetAttachFilter( const std::string& filter ) { m_attachFilter = filter; }

private:
    void Load();
    void Save() const;
    void Rebuild();

    std::string m_fn;

    std::unordered_map<std::string, uint64_t> m_connectionsMap;
    std::vector<std::unordered_map<std::string, uint64_t>::const_iterator> m_connectionsVec;
    std::vector<std::string> m_recentTraces;
    std::vector<AttachHistoryEntry> m_attached;
    std::string m_attachFilter;
};

#endif

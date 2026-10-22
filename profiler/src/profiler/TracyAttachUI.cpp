#include <algorithm>
#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <string>
#include <vector>

#include <imgui.h>

#include "IconsFontAwesome7.h"
#include "TracyAttach.hpp"
#include "TracyAttachUI.hpp"
#include "TracyConfig.hpp"
#include "TracyImGui.hpp"
#include "TracyStorage.hpp"

namespace tracy
{

namespace
{

constexpr ImVec4 Green( 0.2f, 0.75f, 0.3f, 1.f );
constexpr ImVec4 Yellow( 0.95f, 0.75f, 0.1f, 1.f );
constexpr ImVec4 Red( 1.f, 0.3f, 0.3f, 1.f );

struct AttachUIState
{
    AttachSession session;
    std::vector<AttachTarget> targets;
    bool targetsLoaded = false;
    bool filterLoaded = false;
    bool connectSent = false;
    int launch = 0;  // 0 running process, 1 launch new
    int selectedPid = -1;
    char filter[128] = {};
    char hz[16] = "10000";
    char program[1024] = {};
    char args[1024] = {};
    std::string error;
};

AttachUIState& State()
{
    static AttachUIState s;
    return s;
}

bool ContainsNoCase( const std::string& hay, const char* needle )
{
    if( !*needle ) return true;
    const size_t n = strlen( needle );
    return std::search( hay.begin(), hay.end(), needle, needle + n, []( char a, char b ) {
        return tolower( (unsigned char)a ) == tolower( (unsigned char)b );
    } ) != hay.end();
}

std::string Trim( const char* s )
{
    while( isspace( (unsigned char)*s ) ) s++;
    std::string str( s );
    while( !str.empty() && isspace( (unsigned char)str.back() ) ) str.pop_back();
    return str;
}

// Splits on whitespace. Quotes group words and a backslash escapes the next character.
std::vector<std::string> SplitWords( const char* s )
{
    std::vector<std::string> out;
    std::string cur;
    bool inWord = false;
    char quote = 0;
    for( ; *s; s++ )
    {
        const char c = *s;
        if( quote )
        {
            if( c == quote ) quote = 0;
            else if( c == '\\' && quote == '"' && s[1] ) cur += *++s;
            else cur += c;
        }
        else if( c == '"' || c == '\'' )
        {
            quote = c;
            inWord = true;
        }
        else if( c == '\\' && s[1] )
        {
            cur += *++s;
            inWord = true;
        }
        else if( isspace( (unsigned char)c ) )
        {
            if( inWord ) out.push_back( cur );
            cur.clear();
            inWord = false;
        }
        else
        {
            cur += c;
            inWord = true;
        }
    }
    if( inWord ) out.push_back( cur );
    return out;
}

// Recently attached processes, most recent first.
struct AttachHistoryEntry
{
    int pid;
    std::string exe;
    std::string cmdline;
};

constexpr size_t AttachHistoryMax = 10;

std::vector<AttachHistoryEntry>& History()
{
    static std::vector<AttachHistoryEntry> history;
    static bool loaded = false;
    if( !loaded )
    {
        loaded = true;
        if( FILE* f = fopen( GetSavePath( "attach.history" ), "r" ) )
        {
            char line[4096];
            while( fgets( line, sizeof( line ), f ) )
            {
                std::string text = line;
                while( !text.empty() && ( text.back() == '\n' || text.back() == '\r' ) ) text.pop_back();
                const auto t1 = text.find( '\t' );
                if( t1 == std::string::npos ) continue;
                const auto t2 = text.find( '\t', t1 + 1 );
                if( t2 == std::string::npos ) continue;
                history.push_back( AttachHistoryEntry { atoi( text.substr( 0, t1 ).c_str() ), text.substr( t1 + 1, t2 - t1 - 1 ), text.substr( t2 + 1 ) } );
            }
            fclose( f );
        }
    }
    return history;
}

void SaveHistory()
{
    FILE* f = fopen( GetSavePath( "attach.history" ), "w" );
    if( !f ) return;
    for( const auto& h : History() ) fprintf( f, "%d\t%s\t%s\n", h.pid, h.exe.c_str(), h.cmdline.c_str() );
    fclose( f );
}

void RecordAttach( const AttachTarget& target )
{
    auto& history = History();
    history.erase( std::remove_if( history.begin(), history.end(), [&]( const AttachHistoryEntry& e ) {
        return e.pid == target.pid && e.exe == target.exe;
    } ), history.end() );
    history.insert( history.begin(), AttachHistoryEntry { target.pid, target.exe, target.cmdline } );
    if( history.size() > AttachHistoryMax ) history.resize( AttachHistoryMax );
    SaveHistory();
}

// Selects the most recently attached process that still runs, matching pid and path first.
void AutoSelect( AttachUIState& s )
{
    // Without the pid, only the sole process running the executable is picked.
    for( const bool matchPid : { true, false } )
    {
        for( const auto& e : History() )
        {
            if( e.exe.empty() ) continue;
            const AttachTarget* found = nullptr;
            int count = 0;
            for( const auto& t : s.targets )
            {
                if( t.exe == e.exe && ( !matchPid || t.pid == e.pid ) )
                {
                    found = &t;
                    count++;
                }
            }
            if( count == 1 )
            {
                s.selectedPid = found->pid;
                return;
            }
        }
    }
}

void RefreshTargets( AttachUIState& s )
{
    s.targets = ListAttachTargets();
    s.targetsLoaded = true;
    const bool stillThere = std::any_of( s.targets.begin(), s.targets.end(), [&]( const AttachTarget& t ) { return t.pid == s.selectedPid; } );
    if( !stillThere )
    {
        s.selectedPid = -1;
        AutoSelect( s );
    }
}

void StartMonitor( AttachUIState& s, const std::vector<std::string>& monitorArgs )
{
    // The monitor exits with the profiler.
    std::vector<std::string> args { "--exit-with-parent" };
    // The default is not passed, so that TRACY_SAMPLING_HZ still applies.
    const bool hzIsNumber = s.hz[0] && std::all_of( s.hz, s.hz + strlen( s.hz ), []( char c ) { return c >= '0' && c <= '9'; } );
    if( hzIsNumber && strcmp( s.hz, "10000" ) != 0 )
    {
        args.push_back( "--hz" );
        args.push_back( s.hz );
    }
    args.insert( args.end(), monitorArgs.begin(), monitorArgs.end() );

    s.error.clear();
    std::string err;
    if( !s.session.Start( DefaultMonitorPath(), args, err ) ) s.error = err;
    s.connectSent = false;
}

void AttachToPid( AttachUIState& s, int pid )
{
    StartMonitor( s, { "-p", std::to_string( pid ) } );
    if( !s.session.IsRunning() ) return;
    for( const auto& t : s.targets )
    {
        if( t.pid == pid ) { RecordAttach( t ); break; }
    }
}

void LaunchProgram( AttachUIState& s )
{
    const std::string program = Trim( s.program );
    if( program.empty() )
    {
        s.error = "enter a program to launch";
        return;
    }
    std::vector<std::string> words { program };
    const auto argWords = SplitWords( s.args );
    words.insert( words.end(), argWords.begin(), argWords.end() );
    StartMonitor( s, words );
}

void DrawTargetTable( AttachUIState& s, float height )
{
    const ImGuiTableFlags flags = ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg | ImGuiTableFlags_ScrollY;
    if( ImGui::BeginTable( "##attachtargets", 2, flags, ImVec2( 0, height ) ) )
    {
        ImGui::TableSetupScrollFreeze( 0, 1 );
        ImGui::TableSetupColumn( "Name", ImGuiTableColumnFlags_WidthStretch );
        ImGui::TableSetupColumn( "PID", ImGuiTableColumnFlags_WidthFixed, ImGui::GetTextLineHeight() * 5 );
        ImGui::TableHeadersRow();

        for( const auto& t : s.targets )
        {
            if( !ContainsNoCase( t.name, s.filter ) && !ContainsNoCase( t.cmdline, s.filter ) ) continue;

            ImGui::TableNextRow();
            ImGui::PushID( t.pid );
            ImGui::TableNextColumn();
            if( ImGui::Selectable( t.name.c_str(), s.selectedPid == t.pid, ImGuiSelectableFlags_SpanAllColumns | ImGuiSelectableFlags_AllowOverlap ) )
            {
                s.selectedPid = t.pid;
            }
            if( ImGui::IsItemHovered() && ImGui::IsMouseDoubleClicked( ImGuiMouseButton_Left ) && !s.session.IsRunning() ) AttachToPid( s, t.pid );
            if( ImGui::IsItemHovered() )
            {
                if( !t.sameUser ) ImGui::SetTooltip( "Owned by another user: attaching may need elevated privileges.\n%s", t.cmdline.c_str() );
                else if( !t.cmdline.empty() ) ImGui::SetTooltip( "%s", t.cmdline.c_str() );
            }
            ImGui::TableNextColumn();
            if( t.sameUser ) ImGui::Text( "%d", t.pid );
            else ImGui::TextDisabled( "%d", t.pid );
            ImGui::PopID();
        }
        ImGui::EndTable();
    }
}

void SaveFilter( const AttachUIState& s )
{
    tracy::s_config.attachFilter = s.filter;
    tracy::SaveConfig();
}

void DrawFilterRow( AttachUIState& s )
{
    ImGui::SetNextItemWidth( ImGui::GetTextLineHeight() * 10 );
    ImGui::InputTextWithHint( "##attachfilter", ICON_FA_MAGNIFYING_GLASS " Filter", s.filter, sizeof( s.filter ) );
    if( ImGui::IsItemDeactivatedAfterEdit() ) SaveFilter( s );
    ImGui::SameLine();
    if( ImGui::SmallButton( ICON_FA_DELETE_LEFT ) )
    {
        s.filter[0] = 0;
        SaveFilter( s );
    }
    if( ImGui::IsItemHovered() ) ImGui::SetTooltip( "Clear filter" );
    ImGui::SameLine();
    if( ImGui::SmallButton( ICON_FA_ROTATE ) ) RefreshTargets( s );
    if( ImGui::IsItemHovered() ) ImGui::SetTooltip( "Refresh: %d processes", (int)s.targets.size() );
}

void DrawLaunchForm( AttachUIState& s )
{
    const float w = ImGui::GetTextLineHeight() * 24;
    ImGui::SetNextItemWidth( w );
    ImGui::InputText( "Program", s.program, sizeof( s.program ) );
    ImGui::SetNextItemWidth( w );
    ImGui::InputText( "Arguments", s.args, sizeof( s.args ) );
}

void DrawState( const AttachUIState& s )
{
    const auto& session = s.session;
    char buf[128];
    switch( session.GetState() )
    {
    case AttachState::Idle:
        ImGui::TextDisabled( "Not running" );
        break;
    case AttachState::Starting:
        ImGui::TextUnformatted( ICON_FA_SPINNER " Starting tracy-monitor..." );
        break;
    case AttachState::Listening:
        snprintf( buf, sizeof( buf ), ICON_FA_CIRCLE " Waiting for the profiler on 127.0.0.1:%d", (int)session.GetPort() );
        tracy::TextColoredUnformatted( Yellow, buf );
        break;
    case AttachState::Connected:
        snprintf( buf, sizeof( buf ), ICON_FA_CIRCLE_CHECK " Monitor running on 127.0.0.1:%d", (int)session.GetPort() );
        tracy::TextColoredUnformatted( Green, buf );
        break;
    case AttachState::Stopped:
        ImGui::TextUnformatted( "Stopped" );
        break;
    case AttachState::Failed:
        snprintf( buf, sizeof( buf ), ICON_FA_TRIANGLE_EXCLAMATION " Failed (monitor exit status %d)", session.GetExitCode() );
        tracy::TextColoredUnformatted( Red, buf );
        break;
    }
}

void DrawSources( const AttachSession& session )
{
    for( const auto& src : session.GetSources() )
    {
        if( src.state == "yes" || src.state == "warning" )
        {
            const bool ok = src.state == "yes";
            tracy::TextColoredUnformatted( ok ? Green : Yellow, ok ? ICON_FA_CHECK : ICON_FA_TRIANGLE_EXCLAMATION );
            ImGui::SameLine();
            ImGui::TextUnformatted( src.name.c_str() );
        }
        else
        {
            ImGui::TextDisabled( ICON_FA_XMARK " %s", src.name.c_str() );
        }
        if( !src.detail.empty() && ImGui::IsItemHovered() ) ImGui::SetTooltip( "%s", src.detail.c_str() );
    }
}

void DrawLog( const AttachSession& session )
{
    if( session.GetLog().empty() ) return;
    const bool failed = session.GetState() == AttachState::Failed;
    if( !ImGui::CollapsingHeader( "Monitor log", failed ? ImGuiTreeNodeFlags_DefaultOpen : 0 ) ) return;

    ImGui::BeginChild( "##attachlog", ImVec2( 0, ImGui::GetTextLineHeight() * 8 ), ImGuiChildFlags_Borders, ImGuiWindowFlags_HorizontalScrollbar );
    const auto& log = session.GetLog();
    ImGuiListClipper clipper;
    clipper.Begin( (int)log.size() );
    while( clipper.Step() )
    {
        for( int i=clipper.DisplayStart; i<clipper.DisplayEnd; i++ )
        {
            const auto& line = log[i];
            if( line.isError ) tracy::TextColoredUnformatted( Red, line.text.c_str() );
            else ImGui::TextUnformatted( line.text.c_str() );
        }
    }
    ImGui::EndChild();
}

void DrawStatus( AttachUIState& s )
{
    DrawState( s );
    // Only an on-demand monitor accepts another viewer.
    if( s.session.GetState() == AttachState::Connected && s.session.IsOnDemand() )
    {
        ImGui::SameLine();
        if( ImGui::SmallButton( "Reconnect" ) )
        {
            s.session.Reconnect();
            s.connectSent = false;
        }
    }
    if( !s.session.GetTargetName().empty() )
    {
        ImGui::SameLine();
        ImGui::TextDisabled( "%s", s.session.GetTargetName().c_str() );
    }
    if( !s.error.empty() ) tracy::TextColoredUnformatted( Red, s.error.c_str() );

    DrawSources( s.session );
    DrawLog( s.session );
}

}  // namespace

bool PollAttach( const std::function<void( const char* addr, uint16_t port )>& onConnect, bool viewOpen, bool loading )
{
    auto& s = State();
    const auto prevState = s.session.GetState();
    const auto prevLog = s.session.GetLog().size();
    const bool wasConnected = prevState == AttachState::Connected;
    s.session.Poll();
    if( s.session.GetState() == AttachState::Listening && !s.connectSent && !viewOpen && !loading )
    {
        s.connectSent = true;
        onConnect( "127.0.0.1", s.session.GetPort() );
        s.session.MarkConnected();
    }
    // Other monitors serve a single viewer.
    if( wasConnected && !viewOpen && !s.session.IsOnDemand() ) s.session.Stop();
    return s.session.GetState() != prevState || s.session.GetLog().size() != prevLog;
}

void DrawAttachEntry()
{
    auto& s = State();
    if( !s.filterLoaded )
    {
        s.filterLoaded = true;
        snprintf( s.filter, sizeof( s.filter ), "%s", tracy::s_config.attachFilter.c_str() );
    }
    if( !s.targetsLoaded ) RefreshTargets( s );

    const float lh = ImGui::GetTextLineHeight();
    const bool running = s.session.IsRunning();

    ImGui::BeginDisabled( running );
    ImGui::RadioButton( "Running process", &s.launch, 0 );
    ImGui::SameLine();
    ImGui::RadioButton( "Launch new", &s.launch, 1 );
    ImGui::EndDisabled();

    if( s.launch == 0 )
    {
        DrawFilterRow( s );
        DrawTargetTable( s, lh * 10 );
    }
    else
    {
        DrawLaunchForm( s );
    }

    ImGui::BeginDisabled( running );
    ImGui::SetNextItemWidth( lh * 6 );
    ImGui::InputText( "Hz", s.hz, sizeof( s.hz ), ImGuiInputTextFlags_CharsDecimal );
    ImGui::EndDisabled();
    ImGui::SameLine();

    if( running )
    {
        if( ImGui::Button( ICON_FA_STOP " Stop" ) ) s.session.Stop();
    }
    else
    {
        const bool attach = s.launch == 0;
        ImGui::BeginDisabled( attach ? s.selectedPid <= 0 : s.program[0] == 0 );
        if( ImGui::Button( attach ? ICON_FA_LINK " Attach" : ICON_FA_PLAY " Launch" ) )
        {
            if( attach ) AttachToPid( s, s.selectedPid );
            else LaunchProgram( s );
        }
        ImGui::EndDisabled();
    }

    ImGui::Separator();
    DrawStatus( s );
}

}

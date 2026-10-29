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
#include "../History.hpp"
#include "TracyImGui.hpp"

namespace tracy
{

namespace
{

#ifdef __linux__
constexpr bool AttachSupported = true;
#else
constexpr bool AttachSupported = false;
#endif

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
    int launch = 0;  // 0: running process, 1: launch new
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

std::vector<std::string> SplitWords( const char* s )
{
    std::vector<std::string> out;
    std::string cur;
    for( ; *s; s++ )
    {
        if( isspace( (unsigned char)*s ) )
        {
            if( !cur.empty() ) out.push_back( cur );
            cur.clear();
        }
        else
        {
            cur += *s;
        }
    }
    if( !cur.empty() ) out.push_back( cur );
    return out;
}

// Selects the most recent attached process that is still running: same pid and path
// first, then same path alone (the pid may have been reused by another process).
void AutoSelect( AttachUIState& s, const History& history )
{
    for( const bool matchPid : { true, false } )
    {
        for( const auto& e : history.Attached() )
        {
            if( e.exe.empty() ) continue;
            for( const auto& t : s.targets )
            {
                if( t.exe == e.exe && ( !matchPid || t.pid == e.pid ) )
                {
                    s.selectedPid = t.pid;
                    return;
                }
            }
        }
    }
}

void RefreshTargets( AttachUIState& s, const History& history )
{
    s.targets = ListAttachTargets();
    s.targetsLoaded = true;
    // Keep a selection the user made; otherwise look for a process they attached before.
    const bool stillThere = std::any_of( s.targets.begin(), s.targets.end(), [&]( const AttachTarget& t ) { return t.pid == s.selectedPid; } );
    if( !stillThere )
    {
        s.selectedPid = -1;
        AutoSelect( s, history );
    }
}

void StartMonitor( AttachUIState& s, const std::vector<std::string>& monitorArgs )
{
    // The profiler owns the monitor it starts: the monitor exits if the profiler goes away.
    std::vector<std::string> args { "--exit-with-parent" };
    if( s.hz[0] )
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

void AttachToPid( AttachUIState& s, History& history, int pid )
{
    StartMonitor( s, { "-p", std::to_string( pid ) } );
    if( !s.session.IsRunning() ) return;
    for( const auto& t : s.targets )
    {
        if( t.pid == pid ) { history.RecordAttach( { t.pid, t.exe, t.cmdline } ); break; }
    }
}

void LaunchProgram( AttachUIState& s )
{
    auto words = SplitWords( s.program );
    if( words.empty() )
    {
        s.error = "enter a program to launch";
        return;
    }
    auto argWords = SplitWords( s.args );
    words.insert( words.end(), argWords.begin(), argWords.end() );
    StartMonitor( s, words );
}

void DrawTargetTable( AttachUIState& s, History& history, float height )
{
    // A zero width fills the parent, so the list is as wide as the tab it sits in.
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
            // Processes owned by other users need CAP_SYS_PTRACE: show them, but grey them out.
            ImGui::BeginDisabled( !t.sameUser );
            ImGui::TableNextColumn();
            if( ImGui::Selectable( t.name.c_str(), s.selectedPid == t.pid, ImGuiSelectableFlags_SpanAllColumns | ImGuiSelectableFlags_AllowOverlap ) )
            {
                s.selectedPid = t.pid;
            }
            if( ImGui::IsItemHovered() && ImGui::IsMouseDoubleClicked( ImGuiMouseButton_Left ) && !s.session.IsRunning() ) AttachToPid( s, history, t.pid );
            // Tooltips on disabled items need the flag. The command line is only needed now and then.
            if( ImGui::IsItemHovered( ImGuiHoveredFlags_AllowWhenDisabled ) )
            {
                if( !t.sameUser ) ImGui::SetTooltip( "Owned by another user: attaching needs elevated privileges." );
                else if( !t.cmdline.empty() ) ImGui::SetTooltip( "%s", t.cmdline.c_str() );
            }
            ImGui::TableNextColumn();
            ImGui::Text( "%d", t.pid );
            ImGui::EndDisabled();
            ImGui::PopID();
        }
        ImGui::EndTable();
    }
}

// The filter outlives the monitor runs and the profiler restarts.
void SaveFilter( const AttachUIState& s, History& history )
{
    history.SetAttachFilter( s.filter );
}

void DrawFilterRow( AttachUIState& s, History& history )
{
    ImGui::SetNextItemWidth( ImGui::GetTextLineHeight() * 10 );
    if( ImGui::InputTextWithHint( "##attachfilter", ICON_FA_MAGNIFYING_GLASS " Filter", s.filter, sizeof( s.filter ) ) ) SaveFilter( s, history );
    ImGui::SameLine();
    if( ImGui::SmallButton( ICON_FA_DELETE_LEFT ) )
    {
        s.filter[0] = 0;
        SaveFilter( s, history );
    }
    if( ImGui::IsItemHovered() ) ImGui::SetTooltip( "Clear filter" );
    ImGui::SameLine();
    if( ImGui::SmallButton( ICON_FA_ROTATE ) ) RefreshTargets( s, history );
    // The count would widen this row past the Connect tab, so it lives in a tooltip.
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
        // The UI cannot tell whether the viewer is still attached, so the monitor is what is reported.
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

// Without these, a process that is not ours shows up as a silent failure.
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
        // The detail can be long: it goes in a tooltip so the panel keeps its width.
        if( !src.detail.empty() && ImGui::IsItemHovered() ) ImGui::SetTooltip( "%s", src.detail.c_str() );
    }
}

void DrawLog( const AttachSession& session )
{
    // Only meaningful after an attempt; the log is cleared when the next one starts.
    if( session.GetLog().empty() ) return;
    const bool failed = session.GetState() == AttachState::Failed;
    if( !ImGui::CollapsingHeader( "Monitor log", failed ? ImGuiTreeNodeFlags_DefaultOpen : 0 ) ) return;

    // Zero width fills the parent, like the target table.
    ImGui::BeginChild( "##attachlog", ImVec2( 0, ImGui::GetTextLineHeight() * 8 ), ImGuiChildFlags_Borders );
    ImGui::PushTextWrapPos( 0.f );
    for( const auto& line : session.GetLog() )
    {
        if( line.isError ) tracy::TextColoredUnformatted( Red, line.text.c_str() );
        else ImGui::TextUnformatted( line.text.c_str() );
    }
    ImGui::PopTextWrapPos();
    ImGui::EndChild();
}

void DrawStatus( AttachUIState& s )
{
    DrawState( s );
    // Only an on-demand monitor accepts another viewer; the others are stopped when their viewer leaves.
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

void PollAttach( const std::function<void( const char* addr, uint16_t port )>& onConnect, bool viewOpen )
{
    auto& s = State();
    const bool wasConnected = s.session.GetState() == AttachState::Connected;
    s.session.Poll();
    if( s.session.GetState() == AttachState::Listening && !s.connectSent )
    {
        s.connectSent = true;
        onConnect( "127.0.0.1", s.session.GetPort() );
        s.session.MarkConnected();
    }
    // A monitor that is not on-demand serves one viewer only: once that viewer is gone, stop it.
    if( wasConnected && !viewOpen && !s.session.IsOnDemand() ) s.session.Stop();
}

void DrawAttachEntry( History& history )
{
    if( !AttachSupported )
    {
        ImGui::TextDisabled( "Attaching to a local process is only available on Linux for now." );
        return;
    }
    auto& s = State();
    if( !s.filterLoaded )
    {
        s.filterLoaded = true;
        snprintf( s.filter, sizeof( s.filter ), "%s", history.AttachFilter().c_str() );
    }
    if( !s.targetsLoaded ) RefreshTargets( s, history );

    const float lh = ImGui::GetTextLineHeight();
    const bool running = s.session.IsRunning();

    ImGui::BeginDisabled( running );
    ImGui::RadioButton( "Running process", &s.launch, 0 );
    ImGui::SameLine();
    ImGui::RadioButton( "Launch new", &s.launch, 1 );
    ImGui::EndDisabled();

    if( s.launch == 0 )
    {
        DrawFilterRow( s, history );
        DrawTargetTable( s, history, lh * 10 );
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
            if( attach ) AttachToPid( s, history, s.selectedPid );
            else LaunchProgram( s );
        }
        ImGui::EndDisabled();
    }

    ImGui::Separator();
    DrawStatus( s );
}

}

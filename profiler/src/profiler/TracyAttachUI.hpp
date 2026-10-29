#ifndef __TRACYATTACHUI_HPP__
#define __TRACYATTACHUI_HPP__

#include <functional>
#include <stdint.h>

class History;

namespace tracy
{

// Body of the "Monitor" tab of the welcome window: pick a process, attach (runs
// tracy-monitor for it) and follow its status. Call it every frame the tab is shown.
void DrawAttachEntry( History& history );

// Follows the monitor's state. Call it every frame, whether or not the tab is shown.
// onConnect is called once when the monitor is listening. viewOpen tells whether a trace
// is open: a monitor that is not on-demand is stopped once its viewer is gone.
void PollAttach( const std::function<void( const char* addr, uint16_t port )>& onConnect, bool viewOpen );

}

#endif

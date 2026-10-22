#ifndef __TRACYATTACHUI_HPP__
#define __TRACYATTACHUI_HPP__

#include <functional>
#include <stdint.h>

namespace tracy
{

// Body of the "Monitor" tab of the welcome window. Call it every frame the tab is shown.
void DrawAttachEntry();

// Follows the monitor's state, whether or not the tab is shown. onConnect is called once
// when the monitor is listening and no trace is open or loading. A monitor that is not
// on-demand is stopped when its viewer is gone. Returns true when the UI needs a redraw.
bool PollAttach( const std::function<void( const char* addr, uint16_t port )>& onConnect, bool viewOpen, bool loading );

}

#endif

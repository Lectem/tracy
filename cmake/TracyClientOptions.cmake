# Options of the Tracy client. Shared by every project that builds the client (the
# root build and tracy-monitor), so each option is declared once.
#
# Usage:
#   include(cmake/TracyClientOptions.cmake)
#   tracy_apply_client_options(<target>)   # once the target exists
include(${CMAKE_CURRENT_LIST_DIR}/options.cmake)

set_option(TRACY_ENABLE "Enable profiling" OFF)
set_option(TRACY_ON_DEMAND "On-demand profiling" OFF)
set_option_value(TRACY_CALLSTACK "Override the callstack collection depth for tracy zones" "")
set_option_value_as_string(TRACY_PLATFORM_HEADER "Path to a header providing TRACY_HAS_CUSTOM_* hooks for an unsupported platform" "")
set_option(TRACY_NO_CALLSTACK "Disable all callstack related functionality" OFF)
set_option(TRACY_NO_CALLSTACK_INLINES "Disables the inline functions in callstacks" OFF)
set_option(TRACY_ONLY_LOCALHOST "Only listen on the localhost interface" OFF)
set_option(TRACY_NO_BROADCAST "Disable client discovery by broadcast to local network" OFF)
set_option(TRACY_ONLY_IPV4 "Tracy will only accept connections on IPv4 addresses (disable IPv6)" OFF)
set_option(TRACY_NO_CODE_TRANSFER "Disable collection of source code" OFF)
set_option(TRACY_NO_CONTEXT_SWITCH "Disable capture of context switches" OFF)
set_option(TRACY_NO_EXIT "Client executable does not exit until all profile data is sent to server" OFF)
set_option(TRACY_NO_SAMPLING "Disable call stack sampling" OFF)
set_option(TRACY_NO_WAIT_STACKS "Disable capture of context switch call stacks" OFF)
set_option(TRACY_NO_VERIFY "Disable zone validation for C API" OFF)
set_option(TRACY_NO_VSYNC_CAPTURE "Disable capture of hardware Vsync events" OFF)
set_option(TRACY_NO_FRAME_IMAGE "Disable the frame image support and its thread" OFF)
set_option(TRACY_NO_SYSTEM_TRACING "Disable systrace sampling" OFF)
set_option(TRACY_PATCHABLE_NOPSLEDS "Enable nopsleds for efficient patching by system-level tools (e.g. rr)" OFF)
set_option(TRACY_MANUAL_LIFETIME "Enable the manual lifetime management of the profile" OFF)
set_option(TRACY_FIBERS "Enable fibers support" OFF)
set_option(TRACY_NO_CRASH_HANDLER "Disable crash handling" OFF)
set_option(TRACY_TIMER_FALLBACK "Use lower resolution timers" OFF)
set_option(TRACY_DISALLOW_HW_TIMER "Disallow hardware timer (may be useful on VMs). Requires TRACY_TIMER_FALLBACK=ON" OFF)
include(FindPkgConfig)
pkg_check_modules(TRACY_LIBUNWIND_PKG QUIET libunwind)
pkg_check_modules(TRACY_DEBUGINFOD_PKG QUIET libdebuginfod)

# Both default to ON when pkg-config finds the library. -D<option>=OFF overrides that.
if(TRACY_LIBUNWIND_PKG_FOUND)
    set(TRACY_LIBUNWIND_DEFAULT ON)
else()
    set(TRACY_LIBUNWIND_DEFAULT OFF)
endif()
set_option(TRACY_LIBUNWIND_BACKTRACE "Use libunwind backtracing where supported" ${TRACY_LIBUNWIND_DEFAULT})
set_option(TRACY_SYMBOL_OFFLINE_RESOLVE "Instead of full runtime symbol resolution, only resolve the image path and offset to enable offline symbol resolution" OFF)
set_option(TRACY_LIBBACKTRACE_ELF_DYNLOAD_SUPPORT "Enable libbacktrace to support dynamically loaded elfs in symbol resolution resolution after the first symbol resolve operation" OFF)
if(TRACY_DEBUGINFOD_PKG_FOUND)
    set(TRACY_DEBUGINFOD_DEFAULT ON)
else()
    set(TRACY_DEBUGINFOD_DEFAULT OFF)
endif()
set_option(TRACY_DEBUGINFOD "Enable debuginfod support" ${TRACY_DEBUGINFOD_DEFAULT})
set_option(TRACY_IGNORE_MEMORY_FAULTS "Ignore instrumentation errors from memory free events that do not have a matching allocation, or from repeated allocations of the same address" OFF)
set_option(TRACY_OPENGL_AUTO_CALIBRATION "Periodically recalibrate OpenGL GPU/CPU clock drift (forces a CPU/GPU sync each time)" OFF)

# Options that are a plain compile definition when ON.
set(TRACY_CLIENT_FLAG_OPTIONS
    TRACY_ENABLE
    TRACY_ON_DEMAND
    TRACY_NO_CALLSTACK
    TRACY_NO_CALLSTACK_INLINES
    TRACY_ONLY_LOCALHOST
    TRACY_NO_BROADCAST
    TRACY_ONLY_IPV4
    TRACY_NO_CODE_TRANSFER
    TRACY_NO_CONTEXT_SWITCH
    TRACY_NO_EXIT
    TRACY_NO_SAMPLING
    TRACY_NO_WAIT_STACKS
    TRACY_NO_VERIFY
    TRACY_NO_VSYNC_CAPTURE
    TRACY_NO_FRAME_IMAGE
    TRACY_NO_SYSTEM_TRACING
    TRACY_PATCHABLE_NOPSLEDS
    TRACY_MANUAL_LIFETIME
    TRACY_FIBERS
    TRACY_NO_CRASH_HANDLER
    TRACY_TIMER_FALLBACK
    TRACY_DISALLOW_HW_TIMER
    TRACY_LIBUNWIND_BACKTRACE
    TRACY_SYMBOL_OFFLINE_RESOLVE
    TRACY_LIBBACKTRACE_ELF_DYNLOAD_SUPPORT
    TRACY_DEBUGINFOD
    TRACY_IGNORE_MEMORY_FAULTS
    TRACY_OPENGL_AUTO_CALIBRATION
)

# Options that the public headers (public/tracy/) read, so consumers must see them too.
# Everything else only changes how the client itself is compiled.
set(TRACY_CLIENT_CONSUMER_OPTIONS
    TRACY_ENABLE
    TRACY_ON_DEMAND
    TRACY_MANUAL_LIFETIME
    TRACY_FIBERS
    TRACY_OPENGL_AUTO_CALIBRATION
)

function(tracy_apply_client_options target)
    foreach(opt IN LISTS TRACY_CLIENT_FLAG_OPTIONS)
        if(${opt})
            list(FIND TRACY_CLIENT_CONSUMER_OPTIONS ${opt} isConsumerOption)
            if(isConsumerOption EQUAL -1)
                target_compile_definitions(${target} PRIVATE ${opt})
            else()
                target_compile_definitions(${target} PUBLIC ${opt})
            endif()
        endif()
    endforeach()

    # TRACY_CALLSTACK is read by the public headers, so consumers need it.
    if(TRACY_CALLSTACK)
        target_compile_definitions(${target} PUBLIC TRACY_CALLSTACK=${TRACY_CALLSTACK})
    endif()
    if(TRACY_PLATFORM_HEADER)
        target_compile_definitions(${target} PRIVATE "TRACY_PLATFORM_HEADER=\"${TRACY_PLATFORM_HEADER}\"")
    endif()

    # PUBLIC rather than INTERFACE: the client's own sources include these headers.
    if(TRACY_LIBUNWIND_BACKTRACE)
        include(FindPkgConfig)
        pkg_check_modules(unwind REQUIRED libunwind)
        target_include_directories(${target} PUBLIC ${unwind_INCLUDE_DIRS})
        target_link_libraries(${target} PUBLIC ${unwind_LINK_LIBRARIES})
    endif()

    if(TRACY_DEBUGINFOD)
        include(FindPkgConfig)
        pkg_check_modules(debuginfod REQUIRED libdebuginfod)
        target_include_directories(${target} PUBLIC ${debuginfod_INCLUDE_DIRS})
        target_link_libraries(${target} PUBLIC ${debuginfod_LINK_LIBRARIES})
    endif()
endfunction()

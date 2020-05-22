// yabridge: a Wine VST bridge
// Copyright (C) 2020  Robbert van der Helm
//
// This program is free software: you can redistribute it and/or modify
// it under the terms of the GNU General Public License as published by
// the Free Software Foundation, either version 3 of the License, or
// (at your option) any later version.
//
// This program is distributed in the hope that it will be useful,
// but WITHOUT ANY WARRANTY; without even the implied warranty of
// MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
// GNU General Public License for more details.
//
// You should have received a copy of the GNU General Public License
// along with this program.  If not, see <https://www.gnu.org/licenses/>.

#pragma once

#include "../boost-fix.h"

#define NOMINMAX
#define NOSERVICE
#define NOMCX
#define NOIMM
#define WIN32_LEAN_AND_MEAN
#include <vestige/aeffectx.h>
#include <windows.h>

#include <boost/asio/local/stream_protocol.hpp>
#include <mutex>

#include "../../common/logging.h"
#include "../editor.h"
#include "../utils.h"

/**
 * A marker struct to indicate that the editor is about to be opened.
 *
 * @see Vst2Bridge::editor
 */
struct EditorOpening {};

/**
 * This handles the communication between the Linux native VST plugin and the
 * Wine VST host when hosting VST2 plugins. The functions below should be used
 * as callback functions in an `AEffect` object.
 */
class Vst2Bridge {
   public:
    /**
     * Initializes the Windows VST plugin and set up communication with the
     * native Linux VST plugin.
     *
     * @param plugin_dll_path A (Unix style) path to the VST plugin .dll file to
     *   load.
     * @param socket_endpoint_path A (Unix style) path to the Unix socket
     *   endpoint the native VST plugin created to communicate over.
     *
     * @throw std::runtime_error Thrown when the VST plugin could not be loaded,
     *   or if communication could not be set up.
     */
    Vst2Bridge(std::string plugin_dll_path, std::string socket_endpoint_path);

    /**
     * Handle events on the main thread until the plugin quits. This can't be
     * done on another thread since some plugins (e.g. Melda) expect certain
     * (but for some reason not all) events to be passed from the same thread it
     * was initiated from. This is then also the same thread that should handle
     * Win32 GUI events.
     */
    void handle_dispatch();

    // These functions are the entry points for the `*_handler` threads defined
    // below. They're defined here because we can't use lambdas with WinAPI's
    // `CreateThread` which is needed to support the proper call conventions the
    // VST plugins expect.
    void handle_dispatch_midi_events();
    void handle_parameters();
    void handle_process_replacing();

    /**
     * Forward the host callback made by the plugin to the host and return the
     * results.
     */
    intptr_t host_callback(AEffect*, int, int, intptr_t, void*, float);

    /**
     * With the `audioMasterGetTime` host callback the plugin expects the return
     * value from the calblack to be a pointer to a VstTimeInfo struct. If the
     * host did not support a certain time info query, than we'll store the
     * returned null pointer as a nullopt.
     */
    std::optional<VstTimeInfo> time_info;

   private:
    /**
     * A wrapper around `plugin->dispatcher` that handles the opening and
     * closing of GUIs.
     */
    intptr_t dispatch_wrapper(AEffect* plugin,
                              int opcode,
                              int index,
                              intptr_t value,
                              void* data,
                              float option);

    /**
     * The shared library handle of the VST plugin. I sadly could not get
     * Boost.DLL to work here, so we'll just load the VST plugisn by hand.
     */
    std::unique_ptr<std::remove_pointer_t<HMODULE>, decltype(&FreeLibrary)>
        plugin_handle;

    /**
     * The loaded plugin's `AEffect` struct, obtained using the above library
     * handle.
     */
    AEffect* plugin;

    boost::asio::io_context io_context;
    boost::asio::local::stream_protocol::endpoint socket_endpoint;

    // The naming convention for these sockets is `<from>_<to>_<event>`. For
    // instance the socket named `host_vst_dispatch` forwards
    // `AEffect.dispatch()` calls from the native VST host to the Windows VST
    // plugin (through the Wine VST host).

    /**
     * The socket that forwards all `dispatcher()` calls from the VST host to
     * the plugin. This is also used once at startup to populate the values of
     * the `AEffect` object.
     */
    boost::asio::local::stream_protocol::socket host_vst_dispatch;
    /**
     * Used specifically for the `effProcessEvents` opcode. This is needed
     * because the Win32 API is designed to block during certain GUI
     * interactions such as resizing a window or opening a dropdown. Without
     * this MIDI input would just stop working at times.
     */
    boost::asio::local::stream_protocol::socket host_vst_dispatch_midi_events;
    boost::asio::local::stream_protocol::socket vst_host_callback;
    /**
     * Used for both `getParameter` and `setParameter` since they mostly
     * overlap.
     */
    boost::asio::local::stream_protocol::socket host_vst_parameters;
    boost::asio::local::stream_protocol::socket host_vst_process_replacing;

    /**
     * The thread that specifically handles `effProcessEvents` opcodes so the
     * plugin can still receive MIDI during GUI interaction to work around Win32
     * API limitations.
     */
    Win32Thread dispatch_midi_events_handler;
    /**
     * The thread that responds to `getParameter` and `setParameter` requests.
     */
    Win32Thread parameters_handler;
    /**
     * The thread that handles calls to `processReplacing` (and `process`).
     */
    Win32Thread process_replacing_handler;

    /**
     * A binary semaphore to prevent race conditions from the host callback
     * function being called by two threads at once. See `send_event()` for more
     * information.
     */
    std::mutex host_callback_mutex;

    /**
     * A scratch buffer for sending and receiving data during `process` and
     * `processReplacing` calls.
     */
    std::vector<uint8_t> process_buffer;

    /**
     * The MIDI events that have been received **and processed** since the last
     * call to `processReplacing()`. 99% of plugins make a copy of the MIDI
     * events they receive but some plugins such as Kontakt only store pointers
     * to these events, which means that the actual `VstEvent` objects must live
     * at least until the next audio buffer gets processed.
     */
    std::vector<DynamicVstEvents> next_audio_buffer_midi_events;
    /**
     * Mutex for locking the above event queue, since recieving and processing
     * now happens in two different threads.
     */
    std::mutex next_buffer_midi_events_mutex;

    /**
     * The plugin editor window. Allows embedding the plugin's editor into a
     * Wine window, and embedding that Wine window into a window provided by the
     * host. Should be empty when the editor is not open.
     *
     * This field can have three possible states:
     *
     * - `std::nullopt` when the editor is closed.
     * - An `Editor` object when the editor is open.
     * - `EditorOpening` when the editor is not yet open, but the host has
     *   already called `effEditGetRect()` and is about to call `effEditOpen()`.
     *   This is needed because there is a race condition in some bugs that
     *   cause them to crash or enter an infinite Win32 message loop when
     *   `effEditGetRect()` gets dispatched and we then enter the message loop
     *   loop before `effEditOpen()` gets called. Most plugins will handle this
     *   just fine, but a select few plugins make the assumption that the editor
     *   is already open once `effEditGetRect()` has been called, even if
     *   `effEditOpen` has not yet been dispatched. VST hsots on Windows will
     *   call these two events in sequence, so the bug would never occur there.
     *   To work around this we'll use this third state to temporarily stop
     *   processing Windows events in the one or two ticks between these two
     *   events.
     */
    std::variant<std::monostate, Editor, EditorOpening> editor;
};

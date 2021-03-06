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

#include <boost/filesystem.hpp>
#include <boost/process/async_pipe.hpp>
#include <boost/process/environment.hpp>

#include "../common/configuration.h"

/**
 * Boost 1.72 was released with a known breaking bug caused by a missing
 * typedef: https://github.com/boostorg/process/issues/116.
 *
 * Luckily this is easy to fix since it's not really possible to downgrade Boost
 * as it would break other applications.
 *
 * Check if this is still needed for other distros after Arch starts packaging
 * Boost 1.73.
 */
class patched_async_pipe : public boost::process::async_pipe {
   public:
    using boost::process::async_pipe::async_pipe;

    typedef typename handle_type::executor_type executor_type;
};

/**
 * A tag to differentiate between 32 and 64-bit plugins, used to determine which
 * host application to use.
 */
enum class PluginArchitecture { vst_32, vst_64 };

/**
 * Create a logger prefix based on the unique socket path for easy
 * identification. The socket path contains both the plugin's name and a unique
 * identifier.
 *
 * @param socket_path The path to the socket endpoint in use.
 *
 * @return A prefix string for log messages.
 */
std::string create_logger_prefix(const boost::filesystem::path& socket_path);

/**
 * Determine the architecture of a VST plugin (or rather, a .dll file) based on
 * it's header values.
 *
 * See https://docs.microsoft.com/en-us/windows/win32/debug/pe-format for more
 * information on the PE32 format.
 *
 * @param plugin_path The path to the .dll file we're going to check.
 *
 * @return The detected architecture.
 * @throw std::runtime_error If the file is not a .dll file.
 */
PluginArchitecture find_vst_architecture(boost::filesystem::path);

/**
 * Finds the Wine VST host (either `yabridge-host.exe` or `yabridge-host.exe`
 * depending on the plugin). For this we will search in two places:
 *
 *   1. Alongside libyabridge.so if the file got symlinked. This is useful
 *      when developing, as you can simply symlink the the libyabridge.so
 *      file in the build directory without having to install anything to
 *      /usr.
 *   2. In the regular search path.
 *
 * @param plugin_arch The architecture of the plugin, either 64-bit or 32-bit.
 *   Used to determine which host application to use, if available.
 * @param use_plugin_groups Whether the plugin is using plugin groups and we
 *   should be looking for the group host instead of the individual plugin host.
 *
 * @return The a path to the VST host, if found.
 * @throw std::runtime_error If the Wine VST host could not be found.
 */
boost::filesystem::path find_vst_host(PluginArchitecture plugin_arch,
                                      bool use_plugin_groups);

/**
 * Find the VST plugin .dll file that corresponds to this copy of
 * `libyabridge.so`. This should be the same as the name of this file but with a
 * `.dll` file extension instead of `.so`. In case this file does not exist and
 * the `.so` file is a symlink, we'll also repeat this check for the file it
 * links to. This is to support the workflow described in issue #3 where you use
 * symlinks to copies of `libyabridge.so`.
 *
 * @return The a path to the accompanying VST plugin .dll file.
 * @throw std::runtime_error If no matching .dll file could be found.
 */
boost::filesystem::path find_vst_plugin();

/**
 * Locate the Wine prefix this file is located in, if it is inside of a wine
 * prefix. This is done by locating the first parent directory that contains a
 * directory named `dosdevices`.
 *
 * @return Either the path to the Wine prefix (containing the `drive_c?`
 *   directory), or `std::nullopt` if it is not inside of a wine prefix.
 */
std::optional<boost::filesystem::path> find_wineprefix();

/**
 * Generate the group socket endpoint name used based on the name of the group,
 * the Wine prefix in use and the plugin architecture. The resulting format is
 * `/tmp/yabridge-group-<group_name>-<wine_prefix_id>-<architecture>.sock`. In
 * this socket name the `wine_prefix_id` is a numerical hash based on the Wine
 * prefix in use. This way the same group name can be used for multiple Wine
 * prefixes and for both 32 and 64 bit plugins without clashes.
 *
 * @param group_name The name of the plugin group.
 * @param wine_prefix The name of the Wine prefix in use. This should be
 *   obtained by first calling `set_wineprefix()` to allow the user to override
 *   this, and then falling back to `$HOME/.wine` if the environment variable is
 *   unset. Otherwise plugins run from outwide of a Wine prefix will not be
 *   groupable with those run from within `~/.wine` even though they both run
 *   under the same prefix.
 * @param architecture The architecture the plugin is using, since 64-bit
 *   processes can't host 32-bit plugins and the other way around.
 *
 * @return A socket endpoint path that corresponds to the format described
 * above.
 */
boost::filesystem::path generate_group_endpoint(
    const std::string& group_name,
    const boost::filesystem::path& wine_prefix,
    const PluginArchitecture architecture);

/**
 * Generate a unique name for the Unix domain socket endpoint based on the VST
 * plugin's name. This will also generate the parent directory if it does not
 * yet exist since we're using this in the constructor's initializer list.
 *
 * @return A path to a not yet existing Unix domain socket endpoint.
 * @throw std::runtime_error If no matching .dll file could be found.
 */
boost::filesystem::path generate_plugin_endpoint();

/**
 * Return a path to this `.so` file. This can be used to find out from where
 * this link to or copy of `libyabridge.so` was loaded.
 */
boost::filesystem::path get_this_file_location();

/**
 * Return the installed Wine version. This is obtained by from `wine --version`
 * and then stripping the `wine-` prefix. This respects the `WINELOADER`
 * environment variable used in the scripts generated by winegcc.
 *
 * This will *not* throw when Wine can not be found, but will instead return
 * '<NOT FOUND>'. This way the user will still get some useful log files.
 */
std::string get_wine_version();

/**
 * Load the configuration that belongs to a copy of or symlink to
 * `libyabridge.so`. If no configuration file could be found then this will
 * return an empty configuration object with default settings. See the docstrong
 * on the `Configuration` class for more details on how to choose the config
 * file to load.
 *
 * This function will take any optional compile-time features that have not been
 * enabled into account.
 *
 * @param yabridge_path The path to the .so file that's being loaded.by the VST
 *   host. This will be used both for the starting location of the search and to
 *   determine which section in the config file to use.
 *
 * @return Either a configuration object populated with values from matched glob
 *   pattern within the found configuration file, or an empty configuration
 *   object if no configuration file could be found or if the plugin could not
 *   be matched to any of the glob patterns in the configuration file.
 *
 * @see Configuration
 */
Configuration load_config_for(const boost::filesystem::path& yabridge_path);

/**
 * Locate the Wine prefix and set the `WINEPREFIX` environment variable if
 * found. This way it's also possible to run .dll files outside of a Wine prefix
 * using the user's default prefix.
 */
boost::process::environment set_wineprefix();

/**
 * Starting from the starting file or directory, go up in the directory
 * hierarchy until we find a file named `filename`.
 *
 * @param filename The name of the file we're looking for. This can also be a
 *   directory name since directories are also files.
 * @param starting_from The directory to start searching in. If this is a file,
 *   then start searching in the directory the file is located in.
 * @param predicate The predicate to use to check if the path matches a file.
 *   Needed as an easy way to limit the search to directories only since C++17
 *   does not have any built in coroutines or generators.
 *
 * @return The path to the *file* found, or `std::nullopt` if the file could not
 *   be found.
 */
template <typename F = bool(const boost::filesystem::path&)>
std::optional<boost::filesystem::path> find_dominating_file(
    const std::string& filename,
    boost::filesystem::path starting_dir,
    F predicate = boost::filesystem::exists) {
    while (starting_dir != "") {
        const boost::filesystem::path candidate = starting_dir / filename;
        if (predicate(candidate)) {
            return candidate;
        }

        starting_dir = starting_dir.parent_path();
    }

    return std::nullopt;
}

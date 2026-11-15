#pragma once

#include <optional>

// The `orbit` command line: `orbit add <link>`, `orbit list`, `orbit pause --all`, ...
// It talks to the running Orbit over its control socket (a Unix socket in
// $XDG_RUNTIME_DIR that only this user can open), and starts Orbit in the background
// when a download is added while it isn't running.
namespace command_line {

// The socket the running Orbit answers command-line requests on. The browser helper
// only knows the separate API socket, so a web page can never reach these commands.
inline constexpr const char* kControlSocketName = "OrbitDownloadManager.Control";

// Runs the command when argv names one and returns its exit code; returns nothing when
// these arguments are meant for the app itself (no arguments, a link, a .torrent, ...).
std::optional<int> run(int argc, char* argv[]);

}

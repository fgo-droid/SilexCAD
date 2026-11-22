#pragma once
// `silex --doctor`: installation diagnostic printed on the console, without a window.
//
//   [OK]   / [WARN] / [FAIL]  for: embedded resources, OpenCascade + PlaneGCS, the user data
//   folders (lessons, autosave), the IPC/MCP server and the Python runtime of the MCP bridge.
//
// Returns the process exit code: 0 when nothing is blocking, 1 when at least one check failed.

#include <QString>

namespace silex::ui {

int runDoctor(const QString& pipeName);

/// Windows: a GUI-subsystem executable has no console. Attaches to the parent's console (when
/// the output is not already redirected) so that --doctor / --help are readable in a terminal.
void attachParentConsole();

}  // namespace silex::ui

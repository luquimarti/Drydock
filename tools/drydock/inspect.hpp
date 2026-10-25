#pragma once

// drydock --inspect: everything needed to start a new recipe from a disc image.

#include <string>
#include <vector>

namespace drydock {

/// Prints the disc header, the file list and (with memory dumps) where each
/// file was loaded, then a recipe skeleton. Returns a process exit code.
int RunInspect(const std::string &discPath, const std::vector<std::string> &dumpPrefixes);

} // namespace drydock

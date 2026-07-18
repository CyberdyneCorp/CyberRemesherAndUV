#pragma once

#include <string_view>

namespace cyber {

// Semantic version of the engine; single source of truth is the CMake
// project() version, injected at compile time.
[[nodiscard]] std::string_view version();

}  // namespace cyber

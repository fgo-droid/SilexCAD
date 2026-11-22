#pragma once
// Version and source revision, injected by CMake (src/app/CMakeLists.txt).

#ifndef SILEX_VERSION
#define SILEX_VERSION "0.0.0"
#endif
#ifndef SILEX_GIT_COMMIT
#define SILEX_GIT_COMMIT "unknown"
#endif

namespace silex::ui {
inline constexpr const char* kSilexVersion = SILEX_VERSION;
inline constexpr const char* kSilexGitCommit = SILEX_GIT_COMMIT;
}  // namespace silex::ui

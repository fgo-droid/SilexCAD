#pragma once
// Data folders of the tests, resolved at run time so that the test executables keep working when
// the bin/<config> folder is copied elsewhere (RelocationTest):
//
//   resources : $SILEX_RESOURCE_DIR, <exe dir>/resources, then the source tree
//   examples  : $SILEX_RESOURCE_DIR/examples, <exe dir>/resources/examples, then the source tree
//   docs      : <exe dir>/testdata/docs, then the source tree
//   test data : <exe dir>/testdata, then the source tree
//
// With SILEX_PORTABLE_ONLY=1 the source tree is never used (proves the independence from the build).

#include <cstdlib>
#include <filesystem>
#include <initializer_list>
#include <string>

#ifdef _WIN32
#include <windows.h>
#undef near  // <windows.h> macros that collide with ordinary names of the tests
#undef far
#endif

namespace silex::test {

inline std::filesystem::path exeDir()
{
#ifdef _WIN32
    wchar_t buf[32768];
    const DWORD n = GetModuleFileNameW(nullptr, buf, DWORD(std::size(buf)));
    return std::filesystem::path(std::wstring(buf, n)).parent_path();
#else
    std::error_code ec;
    return std::filesystem::read_symlink("/proc/self/exe", ec).parent_path();
#endif
}

inline std::string envVar(const char* name)
{
#ifdef _WIN32
    char* v = nullptr;
    size_t len = 0;
    if (_dupenv_s(&v, &len, name) != 0 || !v)
        return {};
    std::string s(v);
    std::free(v);
    return s;
#else
    const char* v = std::getenv(name);
    return v ? v : "";
#endif
}

inline bool portableOnly()
{
    return envVar("SILEX_PORTABLE_ONLY") == "1";
}

inline std::string pickDir(std::initializer_list<std::filesystem::path> candidates, const char* sourceDir)
{
    std::error_code ec;
    for (const auto& c : candidates)
        if (!c.empty() && std::filesystem::is_directory(c, ec))
            return c.generic_string();
    if (!portableOnly())
        return sourceDir;
    return candidates.size() ? std::data(candidates)[candidates.size() - 1].generic_string() : std::string();
}

inline std::filesystem::path overrideDir(const char* sub = nullptr)
{
    const std::string env = envVar("SILEX_RESOURCE_DIR");
    if (env.empty())
        return {};
    return sub ? std::filesystem::path(env) / sub : std::filesystem::path(env);
}

inline std::string resourcesDir()
{
    return pickDir({overrideDir(), exeDir() / "resources"}, SILEX_SRC_RESOURCES_DIR);
}
inline std::string examplesDir()
{
    return pickDir({overrideDir("examples"), exeDir() / "resources" / "examples"}, SILEX_SRC_EXAMPLES_DIR);
}
inline std::string docsDir()
{
    return pickDir({exeDir() / "testdata" / "docs"}, SILEX_SRC_DOCS_DIR);
}
inline std::string testDataDir()
{
    return pickDir({exeDir() / "testdata"}, SILEX_SRC_TESTDATA_DIR);
}

}  // namespace silex::test

// Historical names used by the tests (they used to be absolute paths baked in at compile time).
#define SILEX_RESOURCES_DIR ::silex::test::resourcesDir()
#define SILEX_EXAMPLES_DIR ::silex::test::examplesDir()
#define SILEX_DOCS_DIR ::silex::test::docsDir()
#define SILEX_TESTDATA_DIR ::silex::test::testDataDir()

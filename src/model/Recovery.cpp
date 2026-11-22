#include "model/Recovery.h"

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cstdio>
#include <ctime>
#include <fstream>
#include <sstream>
#include <stdexcept>

namespace silex::recovery {

namespace fs = std::filesystem;

namespace {
uint64_t fnv1a(const std::string& s)
{
    uint64_t h = 1469598103934665603ull;
    for (unsigned char c : s) {
        h ^= c;
        h *= 1099511628211ull;
    }
    return h;
}

std::string sanitize(const std::string& s)
{
    std::string out;
    for (unsigned char c : s) {
        if (std::isalnum(c) || c == '-' || c == '_')
            out += char(c);
        else if (c >= 0x80)
            out += char(c);  // UTF-8 bytes of accented letters are kept as they are
        else
            out += '_';
        if (out.size() >= 48)
            break;
    }
    return out.empty() ? std::string("untitled") : out;
}

std::string isoUtc(int64_t epoch)
{
    const std::time_t t = static_cast<std::time_t>(epoch);
    std::tm tm{};
#ifdef _WIN32
    gmtime_s(&tm, &t);
#else
    gmtime_r(&t, &tm);
#endif
    char buf[32];
    std::strftime(buf, sizeof buf, "%Y-%m-%dT%H:%M:%SZ", &tm);
    return buf;
}

std::string pathToUtf8(const fs::path& p)
{
    const auto u8 = p.u8string();
    return std::string(u8.begin(), u8.end());
}

Json readJsonFile(const fs::path& file)
{
    std::ifstream in(file, std::ios::binary);
    if (!in)
        throw std::runtime_error("Cannot read " + pathToUtf8(file));
    std::stringstream ss;
    ss << in.rdbuf();
    return Json::parse(ss.str());
}

AutosaveInfo infoFromJson(const Json& j, const fs::path& file)
{
    if (j.value("silex_autosave", 0) < 1 || !j.contains("document"))
        throw std::runtime_error("Not a Silex autosave file: " + pathToUtf8(file));
    AutosaveInfo i;
    i.file = file;
    i.sourcePath = j.value("source_path", std::string{});
    i.displayName = j.value("display_name", std::string{});
    i.savedAt = j.value("saved_at", std::string{});
    i.savedAtEpoch = j.value("saved_at_epoch", int64_t{0});
    i.pid = j.value("pid", int64_t{0});
    return i;
}
}  // namespace

std::string displayNameFor(const std::string& sourcePath)
{
    if (sourcePath.empty())
        return "Untitled";
    const fs::path p(std::u8string(sourcePath.begin(), sourcePath.end()));
    return pathToUtf8(p.stem());
}

std::string autosaveFileName(const std::string& sourcePath, const std::string& sessionKey)
{
    const std::string name = sourcePath.empty() ? std::string("untitled") : sanitize(displayNameFor(sourcePath));
    char hex[17];
    std::snprintf(hex, sizeof hex, "%016llx",
                  static_cast<unsigned long long>(fnv1a(sourcePath.empty() ? "session:" + sessionKey : sourcePath)));
    return name + "_" + std::string(hex, 8) + kSuffix;
}

void writeAutosave(const fs::path& file, const DocState& doc, AutosaveInfo meta)
{
    if (meta.savedAtEpoch == 0)
        meta.savedAtEpoch = std::chrono::duration_cast<std::chrono::seconds>(
                                std::chrono::system_clock::now().time_since_epoch())
                                .count();
    if (meta.savedAt.empty())
        meta.savedAt = isoUtc(meta.savedAtEpoch);
    if (meta.displayName.empty())
        meta.displayName = displayNameFor(meta.sourcePath);

    const Json j{{"silex_autosave", 1},
                 {"saved_at", meta.savedAt},
                 {"saved_at_epoch", meta.savedAtEpoch},
                 {"source_path", meta.sourcePath},
                 {"display_name", meta.displayName},
                 {"pid", meta.pid},
                 {"document", docToJson(doc)}};
    std::error_code ec;
    fs::create_directories(file.parent_path(), ec);
    fs::path tmp = file;
    tmp += ".tmp";
    {
        std::ofstream out(tmp, std::ios::binary | std::ios::trunc);
        if (!out)
            throw std::runtime_error("Cannot write " + pathToUtf8(tmp));
        const std::string text = j.dump(1);
        out.write(text.data(), static_cast<std::streamsize>(text.size()));
        if (!out.flush())
            throw std::runtime_error("Cannot write " + pathToUtf8(tmp));
    }
    fs::rename(tmp, file, ec);  // replaces an older autosave atomically
    if (ec) {
        fs::remove(tmp, ec);
        throw std::runtime_error("Cannot write " + pathToUtf8(file));
    }
}

std::vector<AutosaveInfo> scan(const fs::path& dir)
{
    std::vector<AutosaveInfo> out;
    std::error_code ec;
    if (!fs::is_directory(dir, ec))
        return out;
    const std::string suffix = kSuffix;
    for (const auto& e : fs::directory_iterator(dir, ec)) {
        const std::string name = pathToUtf8(e.path().filename());
        if (!e.is_regular_file(ec) || name.size() <= suffix.size() ||
            name.compare(name.size() - suffix.size(), suffix.size(), suffix) != 0)
            continue;
        try {
            out.push_back(infoFromJson(readJsonFile(e.path()), e.path()));
        }
        catch (...) {
        }
    }
    std::sort(out.begin(), out.end(), [](const AutosaveInfo& a, const AutosaveInfo& b) {
        return a.savedAtEpoch > b.savedAtEpoch;
    });
    return out;
}

DocState readAutosave(const fs::path& file, AutosaveInfo* meta)
{
    const Json j = readJsonFile(file);
    const AutosaveInfo info = infoFromJson(j, file);
    if (meta)
        *meta = info;
    return docFromJson(j.at("document"));
}

}  // namespace silex::recovery

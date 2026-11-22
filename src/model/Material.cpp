#include "model/Material.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdio>

namespace silex {

namespace {
BodyMaterial mk(const char* name, int r, int g, int b, float a, double density)
{
    BodyMaterial m;
    m.name = name;
    m.r = float(r) / 255.f;
    m.g = float(g) / 255.f;
    m.b = float(b) / 255.f;
    m.a = a;
    m.density = density;
    return m;
}

std::string fold(const std::string& s)
{
    // lower-case ASCII, drop non-alphanumerics (accents in UTF-8 are multi-byte: dropped too)
    std::string out;
    for (unsigned char c : s)
        if (std::isalnum(c) && c < 128)
            out.push_back(char(std::tolower(c)));
    return out;
}

std::string hex2(float v)
{
    char buf[3];
    std::snprintf(buf, sizeof buf, "%02X", int(std::lround(std::clamp(v, 0.f, 1.f) * 255.f)));
    return buf;
}

float parseHex2(const std::string& s, size_t at)
{
    return float(std::stoi(s.substr(at, 2), nullptr, 16)) / 255.f;
}
}  // namespace

BodyMaterial defaultMaterial() { return materialPresets().front(); }

const std::vector<BodyMaterial>& materialPresets()
{
    static const std::vector<BodyMaterial> presets{
        mk("PLA Blanc", 0xee, 0xee, 0xe8, 1.f, 1.24),
        mk("PETG Noir", 0x2a, 0x2c, 0x30, 1.f, 1.27),
        mk("ABS Gris", 0x8c, 0x92, 0x9a, 1.f, 1.04),
        mk("R\xC3\xA9sine Translucide", 0xcf, 0xe6, 0xf2, 0.45f, 1.10),
        mk("PCB Vert", 0x1f, 0x7a, 0x3a, 1.f, 1.85),
        mk("Aluminium", 0xc8, 0xcc, 0xd2, 1.f, 2.70),
        mk("Acier", 0x7d, 0x84, 0x8e, 1.f, 7.85),
    };
    return presets;
}

const BodyMaterial* findMaterialPreset(const std::string& nameOrId)
{
    const std::string k = fold(nameOrId);
    if (k.empty())
        return nullptr;
    static const std::vector<std::pair<std::string, int>> aliases{
        {"pla", 0},    {"plablanc", 0},  {"petg", 1},     {"petgnoir", 1},      {"abs", 2},
        {"absgris", 2}, {"resin", 3},    {"resine", 3},   {"rsinetranslucide", 3}, {"resinetranslucide", 3},
        {"pcb", 4},    {"pcbvert", 4},   {"aluminium", 5}, {"aluminum", 5},     {"alu", 5},
        {"acier", 6},  {"steel", 6},
    };
    for (const auto& [alias, i] : aliases)
        if (k == alias)
            return &materialPresets()[size_t(i)];
    for (const auto& m : materialPresets())
        if (fold(m.name) == k)
            return &m;
    return nullptr;
}

void to_json(Json& j, const BodyMaterial& m)
{
    j = Json{{"name", m.name},
             {"color", "#" + hex2(m.r) + hex2(m.g) + hex2(m.b) + hex2(m.a)},
             {"density", m.density}};
}

void from_json(const Json& j, BodyMaterial& m)
{
    m = defaultMaterial();
    m.name = j.value("name", m.name);
    m.density = j.value("density", m.density);
    const std::string c = j.value("color", std::string{});
    if (c.size() == 7 || c.size() == 9) {
        try {
            m.r = parseHex2(c, 1);
            m.g = parseHex2(c, 3);
            m.b = parseHex2(c, 5);
            m.a = c.size() == 9 ? parseHex2(c, 7) : 1.f;
        }
        catch (...) {
        }
    }
}

}  // namespace silex

#include "model/Document.h"

#include <algorithm>
#include <regex>
#include <stdexcept>

namespace silex {

FeaturePtr makeFeature(std::string type, std::string name, Json params, Uuid id)
{
    auto f = std::make_shared<FeatureNode>();
    f->id = id.empty() ? newUuid() : std::move(id);
    f->type = std::move(type);
    f->name = std::move(name);
    f->params = std::move(params);
    return f;
}

int DocState::indexOf(const Uuid& id) const
{
    for (size_t i = 0; i < features.size(); ++i)
        if (features[i]->id == id)
            return static_cast<int>(i);
    return -1;
}

FeaturePtr DocState::find(const Uuid& id) const
{
    const int i = indexOf(id);
    return i < 0 ? nullptr : features[static_cast<size_t>(i)];
}

bool DocState::isActive(const Uuid& id) const
{
    const int i = indexOf(id);
    return i >= 0 && i < marker && !features[static_cast<size_t>(i)]->suppressed;
}

std::string DocState::nextName(const std::string& type) const
{
    for (int n = 1;; ++n) {
        std::string candidate = type + std::to_string(n);
        if (std::none_of(features.begin(), features.end(),
                         [&](const FeaturePtr& f) { return f->name == candidate; }))
            return candidate;
    }
}

// ------------------------------------------------------------------ DAG

std::set<Uuid> dependenciesOf(const FeatureNode& f)
{
    std::set<Uuid> deps;
    static const std::regex uuidRe("[0-9a-f]{8}-[0-9a-f]{4}-[0-9a-f]{4}-[0-9a-f]{4}-[0-9a-f]{12}");
    // Every uuid mentioned anywhere in the parameters (sketch ids, target bodies, persistent
    // names such as "<featId>:side(<sketchId>/<geoId>)") is a dependency, except the
    // feature's own sketch geometry ids which are not feature ids (filtered by the caller).
    const std::string dump = f.params.dump();
    for (auto it = std::sregex_iterator(dump.begin(), dump.end(), uuidRe); it != std::sregex_iterator();
         ++it)
        deps.insert(it->str());
    deps.erase(f.id);
    return deps;
}

std::set<Uuid> dependentsOf(const DocState& s, const Uuid& id)
{
    std::set<Uuid> result{id};
    bool grew = true;
    while (grew) {
        grew = false;
        for (const auto& f : s.features) {
            if (result.count(f->id))
                continue;
            const auto deps = dependenciesOf(*f);
            if (std::any_of(deps.begin(), deps.end(), [&](const Uuid& d) { return result.count(d) > 0; })) {
                result.insert(f->id);
                grew = true;
            }
        }
    }
    result.erase(id);
    return result;
}

// ------------------------------------------------------------------ JSON

Json docToJson(const DocState& s)
{
    Json feats = Json::array();
    for (const auto& f : s.features) {
        Json jf{{"id", f->id}, {"type", f->type}, {"name", f->name}, {"params", f->params}};
        if (f->suppressed)
            jf["suppressed"] = true;
        feats.push_back(std::move(jf));
    }
    return Json{{"format", "silex"},
                {"schema_version", kSchemaVersion},
                {"version", kFileFormatVersion},
                {"marker", s.marker},
                {"features", feats}};
}

namespace {
/// "0.2" -> 2, "0.1" -> 1 (minor number of the 0.x schema line).
int schemaMinor(const std::string& v)
{
    const auto dot = v.find('.');
    if (dot == std::string::npos || v.substr(0, dot) != "0")
        return 1000;  // unknown major: newer
    try {
        return std::stoi(v.substr(dot + 1));
    }
    catch (...) {
        return 1000;
    }
}
}  // namespace

DocState docFromJson(const Json& j)
{
    if (j.value("format", std::string{}) != "silex")
        throw std::runtime_error("Not a Silex document");
    // v0.1 files carry only "version": 1; v0.2+ carry "schema_version": "0.x".
    const int minor = j.contains("schema_version") ? schemaMinor(j["schema_version"].get<std::string>())
                                                   : j.value("version", 1);
    if (minor > kFileFormatVersion)
        throw std::runtime_error("Document was written by a newer Silex version");
    DocState s;
    for (const auto& jf : j.at("features")) {
        auto f = std::make_shared<FeatureNode>();
        f->id = jf.at("id").get<std::string>();
        f->type = jf.at("type").get<std::string>();
        f->name = jf.value("name", f->type);
        f->suppressed = jf.value("suppressed", false);
        f->params = jf.value("params", Json::object());
        s.features.push_back(std::move(f));
    }
    s.marker = std::clamp(j.value("marker", static_cast<int>(s.features.size())), 0,
                          static_cast<int>(s.features.size()));
    return s;
}

// ------------------------------------------------------------------ Commands

DocState AddFeatureCommand::apply(const DocState& s) const
{
    DocState n = s;
    const int at = std::clamp(n.marker, 0, static_cast<int>(n.features.size()));
    n.features.insert(n.features.begin() + at, m_f);
    n.marker = at + 1;
    return n;
}

DocState UpdateFeatureCommand::apply(const DocState& s) const
{
    DocState n = s;
    const int i = n.indexOf(m_f->id);
    if (i >= 0)
        n.features[static_cast<size_t>(i)] = m_f;
    return n;
}

DocState DeleteFeatureCommand::apply(const DocState& s) const
{
    DocState n = s;
    const int i = n.indexOf(m_id);
    if (i < 0)
        return n;
    n.features.erase(n.features.begin() + i);
    if (n.marker > i)
        --n.marker;
    return n;
}

DocState SetSuppressedCommand::apply(const DocState& s) const
{
    DocState n = s;
    const int i = n.indexOf(m_id);
    if (i >= 0) {
        auto f = std::make_shared<FeatureNode>(*n.features[static_cast<size_t>(i)]);
        f->suppressed = m_suppressed;
        n.features[static_cast<size_t>(i)] = std::move(f);
    }
    return n;
}

DocState RenameFeatureCommand::apply(const DocState& s) const
{
    DocState n = s;
    const int i = n.indexOf(m_id);
    if (i >= 0) {
        auto f = std::make_shared<FeatureNode>(*n.features[static_cast<size_t>(i)]);
        f->name = m_name;
        n.features[static_cast<size_t>(i)] = std::move(f);
    }
    return n;
}

DocState MoveMarkerCommand::apply(const DocState& s) const
{
    DocState n = s;
    n.marker = std::clamp(m_marker, 0, static_cast<int>(n.features.size()));
    return n;
}

DocState ReorderFeatureCommand::apply(const DocState& s) const
{
    const int from = s.indexOf(m_id);
    if (from < 0)
        return s;
    DocState n = s;
    FeaturePtr f = n.features[static_cast<size_t>(from)];
    n.features.erase(n.features.begin() + from);
    const int to = std::clamp(m_index, 0, static_cast<int>(n.features.size()));
    n.features.insert(n.features.begin() + to, f);
    // DAG validation: every dependency must still come before its dependents.
    for (size_t i = 0; i < n.features.size(); ++i)
        for (const Uuid& d : dependenciesOf(*n.features[i])) {
            const int di = n.indexOf(d);
            if (di > static_cast<int>(i))
                return s;
        }
    return n;
}

// ------------------------------------------------------------------ History

void History::reset(DocState s)
{
    m_current = std::move(s);
    m_undo.clear();
    m_redo.clear();
    m_dirty = false;
}

void History::execute(const Command& cmd)
{
    m_undo.emplace_back(m_current, cmd.label());
    if (m_undo.size() > kMaxUndo)
        m_undo.erase(m_undo.begin());
    m_redo.clear();
    m_current = cmd.apply(m_current);
    m_dirty = true;
}

bool History::undo()
{
    if (m_undo.empty())
        return false;
    m_redo.emplace_back(m_current, m_undo.back().second);
    m_current = std::move(m_undo.back().first);
    m_undo.pop_back();
    m_dirty = true;
    return true;
}

bool History::redo()
{
    if (m_redo.empty())
        return false;
    m_undo.emplace_back(m_current, m_redo.back().second);
    m_current = std::move(m_redo.back().first);
    m_redo.pop_back();
    m_dirty = true;
    return true;
}

}  // namespace silex

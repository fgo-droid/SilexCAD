#include "ComponentLibrary.h"

#include <QDir>
#include <QFile>
#include <QStandardPaths>
#include <algorithm>
#include <map>
#include <mutex>

#include "ResourceLocator.h"
#include "kernel/KernelEngine.h"
#include "model/Features.h"

namespace silex::ui {

namespace {
std::string text(const Json& j, const char* key, const char* lang)
{
    if (!j.contains(key))
        return {};
    const Json& v = j[key];
    if (v.is_string())
        return v.get<std::string>();
    return v.value(lang, v.value("en", std::string{}));
}
}  // namespace

const std::vector<ComponentInfo>& ComponentLibrary::all()
{
    static const std::vector<ComponentInfo> lib = [] {
        std::vector<ComponentInfo> out;
        for (const QString& f : ResourceLocator::entries("components", {"*.json"})) {
            QFile file(ResourceLocator::path("components/" + f));
            if (!file.open(QIODevice::ReadOnly))
                continue;
            try {
                const Json j = Json::parse(file.readAll().toStdString());
                ComponentInfo c;
                c.id = j.value("id", std::string{});
                c.category = j.value("category", std::string("board"));
                c.step = j.value("step", c.id + ".step");
                c.material = j.value("material", std::string("PCB Vert"));
                c.screw = j.value("screw", std::string{});
                c.standoff = j.value("standoff_mm", 0.0);
                c.nameFr = text(j, "name", "fr");
                c.nameEn = text(j, "name", "en");
                c.descFr = text(j, "description", "fr");
                c.descEn = text(j, "description", "en");
                for (const Json& h : j.value("mounting_holes", Json::array()))
                    c.holes.push_back({h.value("x", 0.0), h.value("y", 0.0), h.value("d", 0.0)});
                c.raw = j;
                if (!c.id.empty())
                    out.push_back(std::move(c));
            }
            catch (...) {
            }
        }
        static const std::vector<std::string> order{"board", "display", "switch", "connector", "fan"};
        auto rank = [](const std::string& cat) {
            const auto it = std::find(order.begin(), order.end(), cat);
            return int(it - order.begin());
        };
        std::stable_sort(out.begin(), out.end(), [&](const ComponentInfo& a, const ComponentInfo& b) {
            return std::make_pair(rank(a.category), a.nameEn) < std::make_pair(rank(b.category), b.nameEn);
        });
        return out;
    }();
    return lib;
}

const ComponentInfo* ComponentLibrary::find(const std::string& id)
{
    for (const auto& c : all())
        if (c.id == id)
            return &c;
    return nullptr;
}

std::optional<FeaturePtr> ComponentLibrary::makeFeature(const DocState& doc, const std::string& id, bool french,
                                                        std::string* error)
{
    auto fail = [&](const std::string& e) -> std::optional<FeaturePtr> {
        if (error)
            *error = e;
        return std::nullopt;
    };
    const ComponentInfo* c = find(id);
    if (!c) {
        std::string ids;
        for (const auto& x : all())
            ids += (ids.empty() ? "" : ", ") + x.id;
        return fail("Unknown component '" + id + "' (" + ids + ")");
    }
    // the B-Rep of each component is read once (STEP -> embedded OCCT text)
    static std::mutex mtx;
    static std::map<std::string, std::string> breps;
    std::string brep;
    {
        std::lock_guard lock(mtx);
        if (auto it = breps.find(id); it != breps.end())
            brep = it->second;
    }
    if (brep.empty()) {
        const QString tmp = QDir(QStandardPaths::writableLocation(QStandardPaths::TempLocation))
                                .filePath(QString("silex_component_%1").arg(QString::fromStdString(c->step)));
        // written from the resource bytes: QFile::copy would keep the read-only flag of the resource
        // and the next insertion could not overwrite the file (Windows)
        QFile res(ResourceLocator::path("components/" + QString::fromStdString(c->step)));
        QFile::setPermissions(tmp, QFileDevice::ReadOwner | QFileDevice::WriteOwner);  // stale read-only copy
        QFile out(tmp);
        if (!res.open(QIODevice::ReadOnly) || !out.open(QIODevice::WriteOnly | QIODevice::Truncate))
            return fail("The model of '" + id + "' is missing from the library");
        out.write(res.readAll());
        out.close();
        try {
            brep = KernelEngine::importStepAsBrep(tmp.toStdString());
        }
        catch (const std::exception& e) {
            QFile::remove(tmp);
            return fail(e.what());
        }
        QFile::remove(tmp);
        std::lock_guard lock(mtx);
        breps[id] = brep;
    }
    ImportParams ip;
    ip.sourcePath = c->step;
    ip.brep = brep;
    ip.component = c->id;
    ip.locked = true;
    ip.material = c->material;
    std::string name = c->name(french);
    int n = 1;
    auto taken = [&](const std::string& s) {
        return std::any_of(doc.features.begin(), doc.features.end(), [&](const FeaturePtr& f) { return f->name == s; });
    };
    while (taken(name))
        name = c->name(french) + " (" + std::to_string(++n) + ")";
    return silex::makeFeature(FeatureType::ImportStep, name, Json(ip));
}

}  // namespace silex::ui

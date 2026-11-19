#pragma once
// Parametric document = immutable state + reducer-style commands (Model-View-Intent).
//
//  UI intent ──► Command (immutable) ──► DocState' = cmd.apply(DocState) ──► History
//                                                         │
//                                                         └─► KernelEngine (async recompute)
//
// DocState values are cheap to copy (features are shared, immutable nodes), which makes
// undo/redo trivial and lets the recompute worker own a consistent snapshot.

#include <memory>
#include <optional>
#include <set>
#include <string>
#include <vector>

#include "model/Features.h"
#include "model/Json.h"
#include "model/Uuid.h"

namespace silex {

struct FeatureNode
{
    Uuid id;
    std::string type;   ///< see FeatureType
    std::string name;   ///< user visible
    bool suppressed = false;
    Json params = Json::object();

    template<typename T>
    T as() const { return params.get<T>(); }
};
using FeaturePtr = std::shared_ptr<const FeatureNode>;

FeaturePtr makeFeature(std::string type, std::string name, Json params, Uuid id = {});

struct DocState
{
    std::vector<FeaturePtr> features;
    int marker = 0;  ///< timeline rollback marker: number of active features

    int indexOf(const Uuid& id) const;
    FeaturePtr find(const Uuid& id) const;
    bool isActive(const Uuid& id) const;
    /// Unique default name such as "Extrude3".
    std::string nextName(const std::string& type) const;
};

/// Explicit dependencies of a feature (DAG edges): referenced sketches, bodies and every
/// feature whose id appears in a persistent topological name it references.
std::set<Uuid> dependenciesOf(const FeatureNode& f);
/// Features (transitively) depending on `id` (excluding itself).
std::set<Uuid> dependentsOf(const DocState& s, const Uuid& id);

Json docToJson(const DocState& s);
DocState docFromJson(const Json& j);
inline constexpr int kFileFormatVersion = 2;            // minor of the 0.x schema
inline constexpr const char* kSchemaVersion = "0.2";

// ------------------------------------------------------------------ Commands

class Command
{
public:
    virtual ~Command() = default;
    virtual std::string label() const = 0;
    virtual DocState apply(const DocState& s) const = 0;
};
using CommandPtr = std::shared_ptr<const Command>;

/// Inserts a feature at the rollback marker (Fusion behaviour) and advances the marker.
class AddFeatureCommand final : public Command
{
public:
    explicit AddFeatureCommand(FeaturePtr f) : m_f(std::move(f)) {}
    std::string label() const override { return "Add " + m_f->name; }
    DocState apply(const DocState& s) const override;

private:
    FeaturePtr m_f;
};

class UpdateFeatureCommand final : public Command
{
public:
    explicit UpdateFeatureCommand(FeaturePtr f) : m_f(std::move(f)) {}
    std::string label() const override { return "Edit " + m_f->name; }
    DocState apply(const DocState& s) const override;

private:
    FeaturePtr m_f;
};

class DeleteFeatureCommand final : public Command
{
public:
    explicit DeleteFeatureCommand(Uuid id) : m_id(std::move(id)) {}
    std::string label() const override { return "Delete feature"; }
    DocState apply(const DocState& s) const override;

private:
    Uuid m_id;
};

class SetSuppressedCommand final : public Command
{
public:
    SetSuppressedCommand(Uuid id, bool suppressed) : m_id(std::move(id)), m_suppressed(suppressed) {}
    std::string label() const override { return m_suppressed ? "Suppress" : "Unsuppress"; }
    DocState apply(const DocState& s) const override;

private:
    Uuid m_id;
    bool m_suppressed;
};

class RenameFeatureCommand final : public Command
{
public:
    RenameFeatureCommand(Uuid id, std::string name) : m_id(std::move(id)), m_name(std::move(name)) {}
    std::string label() const override { return "Rename"; }
    DocState apply(const DocState& s) const override;

private:
    Uuid m_id;
    std::string m_name;
};

class MoveMarkerCommand final : public Command
{
public:
    explicit MoveMarkerCommand(int marker) : m_marker(marker) {}
    std::string label() const override { return "Move timeline marker"; }
    DocState apply(const DocState& s) const override;

private:
    int m_marker;
};

/// Moves a feature to a new timeline position. Refused (no-op) if it would break the DAG.
class ReorderFeatureCommand final : public Command
{
public:
    ReorderFeatureCommand(Uuid id, int newIndex) : m_id(std::move(id)), m_index(newIndex) {}
    std::string label() const override { return "Reorder"; }
    DocState apply(const DocState& s) const override;

private:
    Uuid m_id;
    int m_index;
};

// ------------------------------------------------------------------ History

class History
{
public:
    const DocState& current() const { return m_current; }
    void reset(DocState s);
    void execute(const Command& cmd);
    bool canUndo() const { return !m_undo.empty(); }
    bool canRedo() const { return !m_redo.empty(); }
    bool undo();
    bool redo();
    std::string undoLabel() const { return m_undo.empty() ? "" : m_undo.back().second; }
    std::string redoLabel() const { return m_redo.empty() ? "" : m_redo.back().second; }
    bool dirty() const { return m_dirty; }
    void markClean() { m_dirty = false; }

private:
    DocState m_current;
    std::vector<std::pair<DocState, std::string>> m_undo, m_redo;
    bool m_dirty = false;
    static constexpr size_t kMaxUndo = 500;
};

}  // namespace silex

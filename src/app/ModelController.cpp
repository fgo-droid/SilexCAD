#include "ModelController.h"

#include <QFile>
#include <QMetaObject>
#include <QPointer>
#include <QSaveFile>

#include "sketch/Parametric.h"

namespace silex::ui {

ModelController::ModelController(QObject* parent) : QObject(parent)
{
    QPointer<ModelController> self(this);
    m_engine = std::make_unique<KernelEngine>([self](std::shared_ptr<const ModelSnapshot> snap) {
        // Called on the kernel worker thread -> hop to the UI thread.
        if (!self)
            return;
        QMetaObject::invokeMethod(
            self.data(),
            [self, snap]() {
                if (self)
                    self->onResult(snap);
            },
            Qt::QueuedConnection);
    });
    submit();
}

ModelController::~ModelController()
{
    m_engine.reset();
}

void ModelController::submit()
{
    const bool wasBusy = busy();
    ++m_pending;
    m_engine->submit(displayedDoc(), m_previewState ? m_previewFeature : Uuid{}, m_pending);
    if (!wasBusy)
        emit busyChanged(true);
}

void ModelController::onResult(std::shared_ptr<const ModelSnapshot> snap)
{
    if (snap->revision <= m_received)
        return;  // stale
    m_received = snap->revision;
    m_snapshot = std::move(snap);
    // v0.4 lot 3: failed steps of the committed document are remembered for the AI tutor
    if (!m_previewState)
        for (const auto& r : m_snapshot->results)
            if (r.status == FeatureStatus::Error && !r.message.empty()) {
                const FeaturePtr f = doc().find(r.id);
                const std::string e = (f ? f->name + ": " : std::string()) + r.message;
                if (m_userErrors.empty() || m_userErrors.back() != e)
                    noteUserError(e);
            }
    emit snapshotChanged();
    if (!busy())
        emit busyChanged(false);
}

void ModelController::execute(const Command& cmd)
{
    m_previewState.reset();
    m_previewFeature.clear();
    // v0.3: every change keeps the document parametrically consistent (global parameters ->
    // bound fields -> sketch re-solve), the timeline then recomputes in cascade.
    m_history.execute(ParametricCommand(cmd));
    emit documentChanged();
    submit();
}

void ModelController::undo()
{
    m_previewState.reset();
    if (m_history.undo()) {
        emit documentChanged();
        submit();
    }
}

void ModelController::redo()
{
    m_previewState.reset();
    if (m_history.redo()) {
        emit documentChanged();
        submit();
    }
}

void ModelController::preview(const DocState& state, const Uuid& featureId)
{
    m_previewState = state;
    m_previewFeature = featureId;
    submit();
}

void ModelController::endPreview()
{
    if (!m_previewState)
        return;
    m_previewState.reset();
    m_previewFeature.clear();
    submit();
}

void ModelController::newDocument()
{
    m_previewState.reset();
    m_history.reset(DocState{});
    m_path.clear();
    emit documentChanged();
    submit();
}

bool ModelController::load(const QString& path, QString* error)
{
    QFile f(path);
    if (!f.open(QIODevice::ReadOnly)) {
        if (error)
            *error = f.errorString();
        return false;
    }
    try {
        const QByteArray data = f.readAll();
        // parameters edited by hand in the file are honoured (bound fields re-evaluated)
        DocState s = applyParameters(docFromJson(Json::parse(data.constData(), data.constData() + data.size())));
        m_previewState.reset();
        m_history.reset(std::move(s));
        m_path = path;
        emit documentChanged();
        submit();
        return true;
    }
    catch (const std::exception& e) {
        if (error)
            *error = QString::fromUtf8(e.what());
        return false;
    }
}

void ModelController::restore(DocState state, const QString& sourcePath)
{
    m_previewState.reset();
    m_history.reset(applyParameters(std::move(state)));
    m_history.markDirty();
    m_path = sourcePath;
    emit documentChanged();
    submit();
}

bool ModelController::save(const QString& path, QString* error)
{
    QSaveFile f(path);
    if (!f.open(QIODevice::WriteOnly)) {
        if (error)
            *error = f.errorString();
        return false;
    }
    const std::string text = docToJson(doc()).dump(2);
    f.write(text.data(), qint64(text.size()));
    if (!f.commit()) {
        if (error)
            *error = f.errorString();
        return false;
    }
    m_path = path;
    m_history.markClean();
    emit documentChanged();
    return true;
}

}  // namespace silex::ui

namespace silex::ui {

void ModelController::noteEvent(const std::string& e)
{
    if (m_events.insert(e).second)
        emit eventsChanged();
}

void ModelController::noteUserError(const std::string& e)
{
    if (e.empty())
        return;
    m_userErrors.push_back(e);
    while (m_userErrors.size() > 20)
        m_userErrors.pop_front();
}

}  // namespace silex::ui

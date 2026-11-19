#include "ModelController.h"

#include <QFile>
#include <QMetaObject>
#include <QPointer>
#include <QSaveFile>

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
    emit snapshotChanged();
    if (!busy())
        emit busyChanged(false);
}

void ModelController::execute(const Command& cmd)
{
    m_previewState.reset();
    m_previewFeature.clear();
    m_history.execute(cmd);
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
        DocState s = docFromJson(Json::parse(data.constData(), data.constData() + data.size()));
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

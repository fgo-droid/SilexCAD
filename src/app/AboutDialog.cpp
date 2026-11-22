#include "AboutDialog.h"

#include <QApplication>
#include <QDialogButtonBox>
#include <QFile>
#include <QFont>
#include <QFontDatabase>
#include <QLabel>
#include <QMessageBox>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QTabWidget>
#include <QVBoxLayout>
#include <QtGlobal>

#include "BuildInfo.h"
#include "ResourceLocator.h"
#include "kernel/KernelEngine.h"

namespace silex::ui {

QString AboutDialog::legalText(const QString& name)
{
    // a LICENSE shipped next to the executable wins over the embedded copy
    QString path = ResourceLocator::path("legal/" + name);
    QFile f(path.isEmpty() ? ":/legal/" + name : path);
    if (!f.open(QIODevice::ReadOnly))
        return tr("(%1 is missing from this build)").arg(name);
    return QString::fromUtf8(f.readAll());
}

AboutDialog::AboutDialog(QWidget* parent) : QDialog(parent)
{
    setWindowTitle(tr("About Silex / Licenses"));
    resize(760, 560);
    auto* lay = new QVBoxLayout(this);

    const QString commit = QString::fromLatin1(kSilexGitCommit);
    auto* head = new QLabel(this);
    head->setTextFormat(Qt::RichText);
    head->setTextInteractionFlags(Qt::TextSelectableByMouse);
    head->setText(tr("<h2>Silex %1</h2>"
                     "<p>Open-source parametric 3D CAD for makers and education.</p>"
                     "<p>Source revision: <code>%2</code><br>"
                     "Built with Qt %3 (running %4) and OpenCascade %5, PlaneGCS (FreeCAD), nlohmann/json.</p>"
                     "<p>Silex is free software released under the MIT License. "
                     "The libraries it embeds keep their own licences, listed below.</p>")
                      .arg(QString::fromLatin1(kSilexVersion), commit.toHtmlEscaped(), QStringLiteral(QT_VERSION_STR),
                           QString::fromLatin1(qVersion()), QString::fromStdString(KernelEngine::occtVersion())));
    head->setWordWrap(true);
    lay->addWidget(head);

    auto* tabs = new QTabWidget(this);
    const QFont mono = QFontDatabase::systemFont(QFontDatabase::FixedFont);
    auto addText = [&](const QString& title, const QString& text) {
        auto* edit = new QPlainTextEdit(text, tabs);
        edit->setReadOnly(true);
        edit->setFont(mono);
        edit->setLineWrapMode(QPlainTextEdit::WidgetWidth);
        tabs->addTab(edit, title);
    };
    addText(tr("Silex licence (MIT)"), legalText("LICENSE"));
    addText(tr("Third-party notices"), legalText("THIRD-PARTY-NOTICES.txt"));
    lay->addWidget(tabs, 1);

    auto* buttons = new QDialogButtonBox(QDialogButtonBox::Close, this);
    QPushButton* aboutQt = buttons->addButton(tr("About Qt"), QDialogButtonBox::ActionRole);
    connect(aboutQt, &QPushButton::clicked, this, [this] { QMessageBox::aboutQt(this); });
    connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);
    lay->addWidget(buttons);
}

}  // namespace silex::ui

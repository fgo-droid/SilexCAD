#pragma once
// Help ▾ > About Silex / Licenses: version, Git commit, Silex licence and third-party notices.

#include <QDialog>

namespace silex::ui {

class AboutDialog : public QDialog
{
    Q_OBJECT
public:
    explicit AboutDialog(QWidget* parent = nullptr);
    /// Text of an embedded legal file (":/legal/LICENSE", ":/legal/THIRD-PARTY-NOTICES.txt").
    static QString legalText(const QString& name);
};

}  // namespace silex::ui

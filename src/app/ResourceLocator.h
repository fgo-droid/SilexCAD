#pragma once
// Finds Silex's data files (lessons, components, annotated examples) without depending on the
// build tree. A file "tutorials/01_equerre.json" is looked up, in this order:
//
//   1. $SILEX_RESOURCE_DIR/tutorials/01_equerre.json   (explicit override: ctest, developers)
//   2. <dir of silex.exe>/resources/tutorials/...       (deployed next to the executable)
//   3. :/tutorials/...                                  (copy embedded in the executable)
//
// The embedded copy guarantees that a lone silex.exe still has every critical resource.

#include <QString>
#include <QStringList>

namespace silex::ui::ResourceLocator {

enum class Origin
{
    Override,  ///< $SILEX_RESOURCE_DIR
    Bundle,    ///< <bin>/resources
    Embedded,  ///< Qt resource system
    Missing,
};

/// Existing filesystem roots, in priority order (each one contains tutorials/, components/...).
QStringList roots();
/// Readable path of a resource ("components/rpi4b.step"): a file path or a ":/..." path.
/// Empty when the resource exists nowhere.
QString path(const QString& relative);
Origin originOf(const QString& relative);
/// File names in a resource folder ("tutorials"), merged over every root and the embedded copy,
/// sorted by name. Each name resolves with path(folder + "/" + name).
QStringList entries(const QString& folder, const QStringList& nameFilters);
QString originName(Origin o);

/// v0.7: makes the bundled fonts (resources/fonts) visible to the kernel. An embedded-only copy is
/// extracted to <AppData/Local>/Silex/fonts first (the kernel reads font files from disk).
/// Returns the folder registered (empty if no bundled font was found).
QString registerBundledFonts();

}  // namespace silex::ui::ResourceLocator

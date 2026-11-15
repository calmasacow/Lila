#include "ui/PainterImportUi.h"

#include "core/Log.h"
#include "ui/FileDialog.h"
#include "ui/UiCommands.h"
#include "ui/ViewportToast.h"

#include <QApplication>
#include <QStandardPaths>

#include <filesystem>

namespace lila::ui
{
namespace
{
/** Painter's own smart material folder when it exists, so the picker opens where the files are. */
QString painterSmartMaterialDir()
{
    const std::filesystem::path docs(QStandardPaths::writableLocation(QStandardPaths::DocumentsLocation).toStdWString());
    const std::filesystem::path dir = docs / "Adobe" / "Adobe Substance 3D Painter" / "assets" / "smart-materials";
    std::error_code ec;
    return std::filesystem::is_directory(dir, ec) ? QString::fromStdWString(dir.wstring()) : QString();
}

std::string joined(const json& list)
{
    std::string out;
    for (const json& s : list)
        out += (out.empty() ? "" : ", ") + s.get<std::string>();
    return out;
}
} // namespace

bool isPainterSmartMaterial(const QString& file)
{
    return file.endsWith(".spsm", Qt::CaseInsensitive);
}

int importPainterSmartMaterials(const QStringList& files)
{
    if (files.isEmpty())
        return 0;
    json args = json::array();
    for (const QString& f : files)
        args.push_back(f.toStdString());
    const json r = runUi("texImportPainterSmartMaterials", json{{"args", args}});
    if (!r.is_array())
        return 0;
    int materials = 0, failed = 0, lossy = 0;
    for (const json& f : r)
    {
        const std::string file = std::filesystem::path(f.value("file", "")).filename().string();
        if (f.contains("error"))
        {
            ++failed;
            log::error("{}: {}", file, f["error"].get<std::string>());
            continue;
        }
        materials += int(f.value("materials", json::array()).size());
        if (const json dropped = f.value("dropped", json::array()); !dropped.empty())
        {
            ++lossy;
            log::warn("{}: left out {}", file, joined(dropped));
        }
        if (const json approx = f.value("approximated", json::array()); !approx.empty())
            log::info("{}: approximated {}", file, joined(approx));
    }
    QString text = QString("Imported %1 smart material%2").arg(materials).arg(materials == 1 ? "" : "s");
    if (lossy)
        text += QString(", %1 with parts left out (see log)").arg(lossy);
    if (failed)
        text += QString(", %1 failed").arg(failed);
    viewportToast::showActive(text, 4000, failed ? viewportToast::Kind::Error : lossy ? viewportToast::Kind::Warning : viewportToast::Kind::Info);
    return materials;
}

void importPainterSmartMaterialsDialog()
{
    importPainterSmartMaterials(ui::fileDialog::getOpenFileNames(QApplication::activeWindow(), "Import Painter Smart Materials", painterSmartMaterialDir(),
                                                                 "Painter smart materials (*.spsm)"));
}
} // namespace lila::ui

#pragma once

#include <QStringList>

namespace lila::ui
{
/** Converts Painter .spsm files into smart materials, reports in a toast and the log; returns how many smart materials it made. */
int importPainterSmartMaterials(const QStringList& files);

/** Picks .spsm files (opening in Painter's smart material folder when it exists) and imports them. */
void importPainterSmartMaterialsDialog();

bool isPainterSmartMaterial(const QString& file);
} // namespace lila::ui

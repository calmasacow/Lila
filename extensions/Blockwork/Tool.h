#pragma once

#include "Blockwork.h"

#include <string>

namespace blockwork
{
/** The viewport tool's id and its palette panel's. */
inline constexpr const char* kToolId = "blockwork.tool";
inline constexpr const char* kPaletteId = "blockwork.palette";

enum class Mode
{
    Block,
    Carve,
    Clip,
    Extend,
};

/** What the tool draws and builds, kept in prefs between sessions. */
struct Settings
{
    Mode mode = Mode::Block;
    Shape shape = Shape::Box;
    ShapeOptions options;
    std::string suffix;
    bool flip = false;
    bool removeOverlap = false;
    double hollowThickness = 16.0;
};

Settings& settings();
void saveSettings();
/** Starts the tool in this mode (and shape, for Block and Carve). */
void startTool(Mode mode);
void startTool(Shape shape);
/** Redraws the palette after settings change elsewhere. */
void syncPalette();
const char* modeLabel(Mode m);
const char* shapeLabel(Shape s);

void registerTool(lila::ext::Context& ctx);
void registerPalette(lila::ext::Context& ctx);
} // namespace blockwork

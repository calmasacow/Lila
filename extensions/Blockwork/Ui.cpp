#include "Tool.h"
#include "Entities.h"

#include <lila/Controls.h>
#include <lila/Icons.h>
#include <lila/LevelToolkit.h>
#include <lila/Log.h>
#include <lila/UI.h>
#include <lila/ViewportTool.h>

#include <QCheckBox>
#include <QComboBox>
#include <QDockWidget>
#include <QDoubleSpinBox>
#include <QFormLayout>
#include <QHBoxLayout>
#include <QLineEdit>
#include <QPointer>
#include <QSignalBlocker>
#include <QToolButton>
#include <QVBoxLayout>

#include <array>

namespace blockwork
{
using namespace lila;
using namespace lila::ui;

namespace
{
constexpr const char* kSections = "blockworkPalette";

struct TileDef
{
    const char* label;
    const char* icon;
    const char* tip;
};

constexpr std::array<TileDef, 8> kModeTiles = {{
    {"Block", "box", "draw a block on the grid: click a corner, the opposite corner, then the height"},
    {"Carve", "shovel", "draw a shape the same way and cut it out of every block it overlaps"},
    {"Clip", "slice", "click a line across blocks to cut them along it; Tab picks the side kept"},
    {"Extend", "arrow-bar-up", "drag a block's face to move it in grid steps; Shift extrudes, Ctrl copies it as a new block"},
    {"Paint", "brush", "click or drag over faces to put the picked texture on them; Ctrl+click a face to take its texture"},
    {"Align", "align-box-bottom-center", "click near a face edge to line the texture up with it; W A S D pick the side, Q and E fit"},
    {"Entity", "map-pin", "place a player start, light, trigger or any class from the entity definitions; volumes are drawn like blocks"},
    {"Prefab", "packages", "click to place the prefab picked in the Prefabs panel, stood on the surface; Tab turns it"},
}};

constexpr std::array<TileDef, 6> kShapeTiles = {{
    {"Box", "box", "a box"},
    {"Wedge", "triangle", "a ramp sloping down across the footprint"},
    {"Cylinder", "cylinder", "a cylinder in the footprint; Shift+wheel sets the sides"},
    {"Arch", "building-arch", "a doorway with a round top; Shift+wheel sets the curve's sides"},
    {"Stairs", "stairs", "stairs climbing across the footprint; Shift+wheel sets the steps, Tab turns them"},
    {"Prism", "polygon", "click any outline, concave too, then the height"},
}};

QToolButton* tileButton(const TileDef& d, QWidget* parent)
{
    auto* b = new QToolButton(parent);
    b->setIcon(icon(d.icon));
    b->setIconSize(QSize(24, 24));
    b->setText(d.label);
    b->setToolButtonStyle(Qt::ToolButtonTextUnderIcon);
    b->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
    b->setCheckable(true);
    b->setFocusPolicy(Qt::NoFocus);
    controls::setRole(b, "tile");
    b->setToolTip(QString("%1: %2").arg(d.label, d.tip));
    return b;
}

QFormLayout* form(QWidget* w)
{
    auto* f = new QFormLayout(w);
    f->setContentsMargins(0, 0, 0, 0);
    f->setLabelAlignment(Qt::AlignRight | Qt::AlignVCenter);
    f->setVerticalSpacing(4);
    return f;
}

class Palette final : public QWidget
{
public:
    explicit Palette(QWidget* parent) : QWidget(parent)
    {
        auto* v = new QVBoxLayout(this);
        v->setContentsMargins(12, 8, 12, 8);
        v->setSpacing(4);

        std::vector<QWidget*> modes;
        for (size_t i = 0; i < kModeTiles.size(); ++i)
        {
            mModes[i] = tileButton(kModeTiles[i], this);
            connect(mModes[i], &QToolButton::clicked, this, [i] { startTool(Mode(i)); });
            modes.push_back(mModes[i]);
        }
        v->addWidget(controls::section("Tools", controls::grid(modes, 3, this), this, kSections));

        std::vector<QWidget*> shapes;
        for (size_t i = 0; i < kShapeTiles.size(); ++i)
        {
            mShapes[i] = tileButton(kShapeTiles[i], this);
            connect(mShapes[i], &QToolButton::clicked, this, [i] { startTool(Shape(i)); });
            shapes.push_back(mShapes[i]);
        }
        mShapeSection = controls::section("Shapes", controls::grid(shapes, 3, this), this, kSections);
        v->addWidget(mShapeSection);

        auto* entBox = new QWidget(this);
        auto* entForm = form(entBox);
        mClass = new QComboBox(entBox);
        mClass->setToolTip("Class: what the Entity tool places, from the entity definition files");
        connect(mClass, &QComboBox::activated, this, [this](int) {
            settings().entityClass = mClass->currentData().toString().toStdString();
            saveSettings();
            startTool(Mode::Entity);
        });
        entForm->addRow("Class", mClass);
        auto* entRow = new QWidget(entBox);
        auto* er = new QHBoxLayout(entRow);
        er->setContentsMargins(0, 0, 0, 0);
        er->setSpacing(2);
        for (const char* id : {"blockwork.makeVolume", "blockwork.sendEntities"})
        {
            QToolButton* b = controls::push(id, entRow);
            b->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
            er->addWidget(b);
        }
        entForm->addRow("", entRow);
        mEntitySection = controls::section("Entities", entBox, this, kSections);
        v->addWidget(mEntitySection);

        auto* gridBox = new QWidget(this);
        auto* gridForm = form(gridBox);
        mGrid = controls::valueField(0.125, 4096.0, 3, "cm", gridBox);
        controls::setPresets(mGrid, {0.125, 0.25, 0.5, 1, 2, 4, 8, 16, 32, 64, 128, 256, 512, 1024, 2048, 4096});
        mGrid->setToolTip("Grid: the step every Blockwork tool and move snapping use  ([ and ] while the tool is on)");
        connect(mGrid, &QDoubleSpinBox::valueChanged, this, [](double s) { ops::execute("blockworkGrid", {{"step", s}}, CallSource::UI); });
        gridForm->addRow("Step", mGrid);
        mSnap = new controls::Segmented(gridBox);
        mSnap->add("Lines", {}, "Moves snap onto grid lines", 1);
        mSnap->add("Steps", {}, "Moves snap by whole grid steps from where they start", 0);
        connect(mSnap, &controls::Segmented::picked, this, [](int id) { ops::execute("blockworkGrid", {{"absolute", id == 1}}, CallSource::UI); });
        gridForm->addRow("Snap", mSnap);
        v->addWidget(controls::section("Grid", gridBox, this, kSections));

        mShapeBox = new QWidget(this);
        mShapeForm = form(mShapeBox);
        auto intField = [&](double lo, double hi, const char* tip) {
            QDoubleSpinBox* f = controls::valueField(lo, hi, 0, {}, mShapeBox);
            f->setToolTip(tip);
            return f;
        };
        mSides = intField(3, 256, "Sides: cylinder and arch curve sides  (Shift+wheel while drawing)");
        connect(mSides, &QDoubleSpinBox::valueChanged, this, [](double x) { settings().options.sides = int(x), saveSettings(); });
        mShapeForm->addRow("Sides", mSides);
        mSteps = intField(1, 512, "Steps: stairs step count  (Shift+wheel while drawing)");
        connect(mSteps, &QDoubleSpinBox::valueChanged, this, [](double x) { settings().options.steps = int(x), saveSettings(); });
        mShapeForm->addRow("Steps", mSteps);
        mThickness = controls::valueField(0.0, 1e5, 1, "cm", mShapeBox);
        mThickness->setToolTip("Frame: the arch's width around its opening");
        connect(mThickness, &QDoubleSpinBox::valueChanged, this, [](double x) { settings().options.thickness = x, saveSettings(); });
        mShapeForm->addRow("Frame", mThickness);
        mCorners = new QCheckBox("Corners on Radius", mShapeBox);
        mCorners->setToolTip("Corners on Radius: the cylinder's corners touch the drawn footprint; off, its side centres do");
        connect(mCorners, &QCheckBox::clicked, this, [](bool on) { settings().options.radiusToCorners = on, saveSettings(); });
        mShapeForm->addRow("", mCorners);
        mCaps = new QCheckBox("Caps", mShapeBox);
        mCaps->setToolTip("Caps: close the cylinder's ends");
        connect(mCaps, &QCheckBox::clicked, this, [](bool on) { settings().options.skipCaps = !on, saveSettings(); });
        mShapeForm->addRow("", mCaps);
        mShapeSettings = controls::section("Shape", mShapeBox, this, kSections);
        v->addWidget(mShapeSettings);

        auto* buildBox = new QWidget(this);
        auto* buildForm = form(buildBox);
        mSuffix = new QLineEdit(buildBox);
        mSuffix->setPlaceholderText("none");
        mSuffix->setToolTip("Suffix: added to new blocks' names, such as _col for engines that read collision from names");
        connect(mSuffix, &QLineEdit::editingFinished, this, [this] { settings().suffix = mSuffix->text().trimmed().toStdString(), saveSettings(); });
        buildForm->addRow("Suffix", mSuffix);
        mFlip = new QCheckBox("Faces Inward", buildBox);
        mFlip->setToolTip("Faces Inward: new blocks face inside, for rooms seen from within");
        connect(mFlip, &QCheckBox::clicked, this, [](bool on) { settings().flip = on, saveSettings(); });
        buildForm->addRow("", mFlip);
        mOverlap = new QCheckBox("Remove Touching Faces", buildBox);
        mOverlap->setToolTip("Remove Touching Faces: faces of a new block pressed against another block's go, from both");
        connect(mOverlap, &QCheckBox::clicked, this, [](bool on) { settings().removeOverlap = on, saveSettings(); });
        buildForm->addRow("", mOverlap);
        v->addWidget(controls::section("New Blocks", buildBox, this, kSections, false));

        auto* edit = new QWidget(this);
        auto* editV = new QVBoxLayout(edit);
        editV->setContentsMargins(0, 0, 0, 0);
        editV->setSpacing(2);
        std::vector<QWidget*> rows = {controls::row("blockwork.hollow", "box-multiple", edit), controls::row("blockwork.join", "box", edit),
                                      controls::row("blockwork.flip", "arrows-exchange", edit)};
        editV->addWidget(controls::grid(rows, 2, edit));
        auto* hollowForm = form(new QWidget(edit));
        mHollow = controls::valueField(0.01, 1e5, 1, "cm", edit);
        mHollow->setToolTip("Walls: the thickness Hollow gives the walls");
        connect(mHollow, &QDoubleSpinBox::valueChanged, this, [](double x) { settings().hollowThickness = x, saveSettings(); });
        hollowForm->addRow("Walls", mHollow);
        editV->addWidget(hollowForm->parentWidget());
        v->addWidget(controls::section("Blocks", edit, this, kSections));
        v->addStretch();
        sync();
    }

    void sync()
    {
        const Settings& s = settings();
        const bool active = viewportTools::activeId() == kToolId;
        for (size_t i = 0; i < mModes.size(); ++i)
            mModes[i]->setChecked(active && int(s.mode) == int(i));
        const bool shaped = s.mode == Mode::Block || s.mode == Mode::Carve;
        for (size_t i = 0; i < mShapes.size(); ++i)
            mShapes[i]->setChecked(shaped && int(s.shape) == int(i));
        mShapeSection->setEnabled(shaped);
        mEntitySection->setVisible(s.mode == Mode::Entity);
        {
            const QSignalBlocker b(mClass);
            mClass->clear();
            for (const EntityClass& c : entityClasses())
                mClass->addItem(QString("%1  (%2)").arg(QString::fromStdString(c.name), c.kind == "volume" ? "volume" : "point"), QString::fromStdString(c.name));
            mClass->setCurrentIndex(std::max(0, mClass->findData(QString::fromStdString(s.entityClass))));
        }
        for (auto [box, value] : {std::pair<QDoubleSpinBox*, double>{mGrid, gridStep()}, {mSides, double(s.options.sides)},
                                  {mSteps, double(s.options.steps)}, {mThickness, s.options.thickness}, {mHollow, s.hollowThickness}})
        {
            const QSignalBlocker block(box);
            box->setValue(value);
        }
        {
            const QSignalBlocker b(mSnap);
            mSnap->setCurrent(gridAbsolute() ? 1 : 0);
        }
        mCorners->setChecked(s.options.radiusToCorners);
        mCaps->setChecked(!s.options.skipCaps);
        mSuffix->setText(QString::fromStdString(s.suffix));
        mFlip->setChecked(s.flip);
        mOverlap->setChecked(s.removeOverlap);
        // Only the drawn shape's own settings show.
        const bool round = s.shape == Shape::Cylinder || s.shape == Shape::Arch;
        const std::pair<QWidget*, bool> rows[] = {{mSides, round}, {mSteps, s.shape == Shape::Stairs}, {mThickness, s.shape == Shape::Arch},
                                                  {mCorners, s.shape == Shape::Cylinder}, {mCaps, s.shape == Shape::Cylinder}};
        bool any = false;
        for (auto [w, shown] : rows)
        {
            mShapeForm->setRowVisible(w, shown && shaped);
            any = any || (shown && shaped);
        }
        mShapeSettings->setVisible(any);
    }

private:
    std::array<QToolButton*, kModeTiles.size()> mModes{};
    std::array<QToolButton*, kShapeTiles.size()> mShapes{};
    QWidget *mShapeSection = nullptr, *mShapeBox = nullptr, *mShapeSettings = nullptr;
    QFormLayout* mShapeForm = nullptr;
    QDoubleSpinBox *mGrid = nullptr, *mSides = nullptr, *mSteps = nullptr, *mThickness = nullptr, *mHollow = nullptr;
    controls::Segmented* mSnap = nullptr;
    QCheckBox *mCorners = nullptr, *mCaps = nullptr, *mFlip = nullptr, *mOverlap = nullptr;
    QLineEdit* mSuffix = nullptr;
    QComboBox* mClass = nullptr;
    QWidget* mEntitySection = nullptr;
};

QPointer<Palette> gPalette;

void runSelected(const char* cmd, json call)
{
    try
    {
        ops::execute(cmd, call, CallSource::UI);
    }
    catch (const std::exception& e)
    {
        log::warn("{}", e.what());
    }
}
} // namespace

void syncPalette()
{
    if (gPalette)
        gPalette->sync();
}

void registerPalette(ext::Context& ctx)
{
    panels::add(ctx, {.id = kPaletteId,
                      .title = "Blockwork",
                      .create = [](QWidget* parent) -> QWidget* { return gPalette = new Palette(parent); },
                      .area = Qt::LeftDockWidgetArea,
                      .icon = "box",
                      .open = false,
                      .place = [](QDockWidget* d) { d->setProperty(panels::kOwnSizeProperty, true); },
                      .floatFirst = true,
                      .toolPalette = true});
    const int listener = viewportTools::addListener([] { syncPalette(); });
    ctx.track("blockwork palette sync", [listener] { viewportTools::removeListener(listener); });
}

void registerUi(ext::Context& ctx)
{
    registerTool(ctx);
    registerPalette(ctx);
    registerTexturePanel(ctx);
    registerOrganisePanels(ctx);

    const std::vector<std::string> keywords = {"blockout", "greybox", "graybox", "brush", "hammer", "trenchbroom", "level design", "csg"};
    for (int i = 0; i < int(kModeTiles.size()); ++i)
    {
        const Mode m = Mode(i);
        static const char* const ids[] = {"blockwork.block", "blockwork.carve", "blockwork.clip", "blockwork.extend", "blockwork.paint", "blockwork.align", "blockwork.entity", "blockwork.prefab"};
        actions::add(ctx, {.id = ids[i],
                           .label = std::string(m == Mode::Block ? "Block" : modeLabel(m)) + " Tool",
                           .category = "Blockwork",
                           .run = [m] { startTool(m); },
                           .doc = std::string(kModeTiles[size_t(i)].label) + ": " + kModeTiles[size_t(i)].tip + ".",
                           .checked = [m] { return viewportTools::activeId() == kToolId && settings().mode == m; },
                           .icon = kModeTiles[size_t(i)].icon,
                           .keywords = keywords});
    }
    for (int i = 0; i < int(kShapeTiles.size()); ++i)
        actions::add(ctx, {.id = "blockwork.shape." + shapeNames()[size_t(i)],
                           .label = std::string(kShapeTiles[size_t(i)].label) + " Block",
                           .category = "Blockwork",
                           .run = [i] { startTool(Shape(i)); },
                           .doc = std::string("Draw ") + kShapeTiles[size_t(i)].tip + " on the grid.",
                           .icon = kShapeTiles[size_t(i)].icon,
                           .keywords = keywords});
    actions::add(ctx, {.id = "blockwork.hollow",
                       .label = "Hollow",
                       .category = "Blockwork",
                       .run = [] { runSelected("blockworkHollow", {{"thickness", settings().hollowThickness}}); },
                       .doc = "Turn the selected blocks into walls around their volume; selected faces are left open.",
                       .python = "blockworkHollow('block1', thickness=16)",
                       .icon = "box-multiple",
                       .keywords = {"shell", "room", "solidify"}});
    actions::add(ctx, {.id = "blockwork.join",
                       .label = "Join Blocks",
                       .category = "Blockwork",
                       .doc = "Merge the selected blocks into the first, removing faces where they meet; one block joins whatever touches it.",
                       .python = "blockworkJoin(['room1', 'corridor1'])",
                       .command = "blockworkJoin",
                       .call = json::object(),
                       .icon = "box",
                       .keywords = {"weld", "merge", "union"}});
    actions::add(ctx, {.id = "blockwork.flip",
                       .label = "Flip Blocks",
                       .category = "Blockwork",
                       .doc = "Turn the selected blocks inside out, or just their selected faces.",
                       .python = "blockworkFlip('room1')",
                       .command = "blockworkFlip",
                       .call = json::object(),
                       .icon = "arrows-exchange",
                       .keywords = {"invert", "reverse normals"}});
    actions::add(ctx, {.id = "blockwork.makeVolume",
                       .label = "Make Volume",
                       .category = "Blockwork",
                       .run = [] { runSelected("blockworkVolume", {{"class", settings().entityClass}}); },
                       .doc = "Turn the selected blocks into volumes of the Entity tool's class (a volume class such as Trigger).",
                       .python = "blockworkVolume('block3', class='Trigger')",
                       .icon = "box",
                       .keywords = {"trigger", "volume", "brush entity"}});
    actions::add(ctx, {.id = "blockwork.sendEntities",
                       .label = "Send Entities",
                       .category = "Blockwork",
                       .doc = "Send the selected entities, or every entity, to the open Unreal level as their classes' actors.",
                       .python = "blockworkSendEntities()",
                       .command = "blockworkSendEntities",
                       .call = json::object(),
                       .icon = "send",
                       .keywords = {"unreal", "actors", "spawn"}});
    actions::add(ctx, {.id = "blockwork.gridFiner",
                       .label = "Finer Grid",
                       .category = "Blockwork",
                       .doc = "Halve the Blockwork grid step.",
                       .command = "blockworkGrid",
                       .call = {{"finer", true}},
                       .icon = "grid-dots"});
    actions::add(ctx, {.id = "blockwork.gridCoarser",
                       .label = "Coarser Grid",
                       .category = "Blockwork",
                       .doc = "Double the Blockwork grid step.",
                       .command = "blockworkGrid",
                       .call = {{"coarser", true}},
                       .icon = "grid-3x3"});

    levelToolkit::add(ctx, {.id = "blockwork",
                            .title = "Blockwork",
                            .items = {{"blockwork.block", "box", "Block", "Block"},
                                      {"blockwork.carve", "shovel", "Carve"},
                                      {"blockwork.clip", "slice", "Clip"},
                                      {"blockwork.extend", "arrow-bar-up", "Extend"},
                                      {"blockwork.paint", "brush", "Paint", {}, "Texture"},
                                      {"blockwork.align", "align-box-bottom-center", "Align", {}, "Texture"},
                                      {"blockwork.textures", "texture", "Textures", {}, "Texture"},
                                      {"blockwork.entity", "map-pin", "Entity", {}, "Entities"},
                                      {"blockwork.makeVolume", "box", "Volume", {}, "Entities"},
                                      {"blockwork.sendEntities", "send", "Send", {}, "Entities"},
                                      {"blockwork.prefabs", "packages", "Prefabs", {}, "Organise"},
                                      {"blockwork.visgroups", "eye", "Visgroups", {}, "Organise"},
                                      {"blockwork.merge", "box-multiple", "Merge", {}, "Organise"},
                                      {"blockwork.toolMaterials", "texture", "Tool Faces", {}, "Organise", true},
                                      {"blockwork.hollow", "box-multiple", "Hollow"},
                                      {"blockwork.join", "box", "Join"},
                                      {"blockwork.flip", "arrows-exchange", "Flip", {}, {}, true}},
                            .order = 5,
                            .docs = "blockwork"});
    menus::addItem(ctx, {.id = "blockwork.menu", .menu = levelToolkit::kMenu, .kind = menus::Kind::Submenu, .label = "Blockwork",
                         .before = levelToolkit::kDressSection});
    for (const char* a : {"blockwork.block", "blockwork.carve", "blockwork.clip", "blockwork.extend", "blockwork.paint", "blockwork.align", "blockwork.textures", "blockwork.entity", "blockwork.makeVolume", "blockwork.sendEntities", "blockwork.prefabs", "blockwork.visgroups", "blockwork.merge", "blockwork.toolMaterials"})
        menus::addItem(ctx, {.id = std::string("blockwork.menu.") + a, .menu = "Level/Blockwork", .action = a});
    menus::addItem(ctx, {.id = "blockwork.menu.sep1", .menu = "Level/Blockwork", .kind = menus::Kind::Separator});
    for (const std::string& s : shapeNames())
        menus::addItem(ctx, {.id = "blockwork.menu.shape." + s, .menu = "Level/Blockwork", .action = "blockwork.shape." + s});
    menus::addItem(ctx, {.id = "blockwork.menu.sep2", .menu = "Level/Blockwork", .kind = menus::Kind::Separator});
    for (const char* a : {"blockwork.hollow", "blockwork.join", "blockwork.flip", "blockwork.gridFiner", "blockwork.gridCoarser"})
        menus::addItem(ctx, {.id = std::string("blockwork.menu.") + a, .menu = "Level/Blockwork", .action = a});
}
} // namespace blockwork

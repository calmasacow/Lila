#include <doctest/doctest.h>

#include "ops/Command.h"
#include "scene/Graph.h"
#include "scene/Node.h"
#include "scene/SceneCommands.h"
#include "scene/Transform.h"
#include "ui/Actions.h"
#include "ui/DockSidebar.h"
#include "ui/Menus.h"
#include "ui/Theme.h"
#include "ui/Shelf.h"
#include "ui/TearOff.h"
#include "ui/TearOffLogic.h"
#include "ui/ViewportHooks.h"

#include <QAction>
#include <QApplication>
#include <QCursor>
#include <QDockWidget>
#include <QElapsedTimer>
#include <QKeyEvent>
#include <QMainWindow>
#include <QMenuBar>
#include <QDropEvent>
#include <QLabel>
#include <QMimeData>
#include <QMouseEvent>
#include <QPushButton>
#include <QStyleFactory>
#include <QScreen>
#include <QStyle>
#include <QWindow>

#if defined(Q_OS_LINUX) && QT_CONFIG(xcb)
#include <QtGui/qguiapplication_platform.h>
#include <xcb/xcb.h>
#endif

using namespace lila::ui;

namespace
{
/** Visible rows of a torn-off panel, by label. */
std::vector<QPushButton*> rows(QDockWidget* dock)
{
    std::vector<QPushButton*> out;
    for (QPushButton* b : dock->widget()->findChildren<QPushButton*>("tearOffRow"))
        if (!b->isHidden() && !b->parentWidget()->isHidden())
            out.push_back(b);
    return out;
}

QPushButton* row(QDockWidget* dock, const QString& label)
{
    for (QPushButton* b : rows(dock))
        if (b->text() == label)
            return b;
    return nullptr;
}

void settleBriefly()
{
    for (int i = 0; i < 3; ++i)
        QApplication::processEvents();
}

void wait(int ms)
{
    QElapsedTimer t;
    t.start();
    while (t.elapsed() < ms)
        QApplication::processEvents(QEventLoop::AllEvents, 5);
}
/** Records the action id of a drop from a menu drag. */
class DropCatcher : public QWidget
{
public:
    DropCatcher() { setAcceptDrops(true); }
    QString dropped;

protected:
    void dragEnterEvent(QDragEnterEvent* e) override { e->setAccepted(e->mimeData()->hasFormat(tearoff::kActionMime)); }
    void dragMoveEvent(QDragMoveEvent* e) override { e->accept(); }
    void dropEvent(QDropEvent* e) override { dropped = QString::fromUtf8(e->mimeData()->data(tearoff::kActionMime)); }
};
} // namespace

TEST_CASE("a torn-off menu is a dock of buttons bound to the menu's actions")
{
    QApplication::setStyle(QStyleFactory::create("Fusion"));
    QMainWindow window;
    window.setCentralWidget(new QWidget);
    QMenu* mesh = tearoff::enable(window.menuBar()->addMenu("&Mesh"));
    int extrudes = 0;
    QAction* extrude = mesh->addAction("&Extrude\tCtrl+E");
    QObject::connect(extrude, &QAction::triggered, [&] { ++extrudes; });
    QAction* toggle = mesh->addAction("Symmetry On");
    toggle->setCheckable(true);
    QAction* disabled = mesh->addAction("Disabled");
    disabled->setEnabled(false);
    mesh->addSeparator();
    QMenu* symmetry = mesh->addMenu("Symmetry");
    symmetry->addAction("X");
    window.show();
    settleBriefly();

    CHECK(mesh->isTearOffEnabled());
    CHECK_FALSE(symmetry->isTearOffEnabled());
    // Submenus get the row as their parent opens, so ones added later are covered too.
    Q_EMIT mesh->aboutToShow();
    CHECK(symmetry->isTearOffEnabled());
    REQUIRE(tearoff::pathOf(symmetry));
    CHECK(*tearoff::pathOf(symmetry) == std::vector<std::string>{"Mesh", "Symmetry"});
    CHECK(tearoff::findMenu(&window, {"Mesh", "Symmetry"}) == symmetry);
    CHECK(tearoff::findMenu(&window, {"Mesh", "Nope"}) == nullptr);

    QDockWidget* dock = tearoff::tearOff(mesh, QPoint(200, 200));
    REQUIRE(dock);
    settleBriefly();
    CHECK(dock->objectName() == "tearoff:Mesh");
    CHECK(dock->isFloating());
    CHECK(dock->windowTitle() == "Mesh");
    CHECK(tearoff::tearOff(mesh) == dock);

    REQUIRE(row(dock, "Extrude"));
    row(dock, "Extrude")->click();
    CHECK(extrudes == 1);
    REQUIRE(row(dock, "Disabled"));
    CHECK_FALSE(row(dock, "Disabled")->isEnabled());
    REQUIRE(row(dock, "Symmetry"));

    SUBCASE("rows follow the actions' state and the menu's contents")
    {
        disabled->setEnabled(true);
        CHECK(row(dock, "Disabled")->isEnabled());
        extrude->setText("Extrude Faces");
        CHECK(row(dock, "Extrude Faces"));
        disabled->setVisible(false);
        CHECK_FALSE(row(dock, "Disabled"));
        size_t before = rows(dock).size();
        mesh->addAction("Bridge");
        settleBriefly();
        CHECK(rows(dock).size() == before + 1);
        CHECK(row(dock, "Bridge"));
        mesh->removeAction(toggle);
        settleBriefly();
        CHECK_FALSE(row(dock, "Symmetry On"));
    }

    SUBCASE("clicking the dashed row tears the menu off instead of Qt's floating menu")
    {
        mesh->popup(QPoint(50, 50));
        settleBriefly();
        QPoint at(mesh->width() / 2, mesh->actionGeometry(extrude).top() / 2);
        QMouseEvent press(QEvent::MouseButtonPress, at, mesh->mapToGlobal(at), Qt::LeftButton, Qt::LeftButton, Qt::NoModifier);
        QApplication::sendEvent(mesh, &press);
        QMouseEvent release(QEvent::MouseButtonRelease, at, mesh->mapToGlobal(at), Qt::LeftButton, Qt::NoButton, Qt::NoModifier);
        QApplication::sendEvent(mesh, &release);
        settleBriefly();
        CHECK_FALSE(mesh->isTearOffMenuVisible());
        CHECK_FALSE(mesh->isVisible());
        QDockWidget* sym = nullptr;
        symmetry->popup(QPoint(50, 50));
        settleBriefly();
        QPoint symAt(symmetry->width() / 2, symmetry->actionGeometry(symmetry->actions().front()).top() / 2);
        QMouseEvent p2(QEvent::MouseButtonPress, symAt, symmetry->mapToGlobal(symAt), Qt::LeftButton, Qt::LeftButton, Qt::NoModifier);
        QApplication::sendEvent(symmetry, &p2);
        QMouseEvent r2(QEvent::MouseButtonRelease, symAt, symmetry->mapToGlobal(symAt), Qt::LeftButton, Qt::NoButton, Qt::NoModifier);
        QApplication::sendEvent(symmetry, &r2);
        settleBriefly();
        sym = window.findChild<QDockWidget*>("tearoff:Mesh/Symmetry");
        REQUIRE(sym);
        CHECK(row(sym, "X"));
    }

    SUBCASE("a layout recreates torn-off docks by name")
    {
        tearoff::recreate(&window, {"tearoff:Mesh/Symmetry", "tearoff:Missing", "outliner"});
        QDockWidget* sym = window.findChild<QDockWidget*>("tearoff:Mesh/Symmetry");
        REQUIRE(sym);
        CHECK(row(sym, "X"));
        CHECK_FALSE(window.findChild<QDockWidget*>("tearoff:Missing"));
        tearoff::recreate(&window, {"tearoff:Mesh/Symmetry"});
        CHECK(window.findChildren<QDockWidget*>("tearoff:Mesh/Symmetry").size() == 1);
    }
}

TEST_CASE("clicking a section title tears off only that section")
{
    QApplication::setStyle(QStyleFactory::create("Fusion"));
    QMainWindow window;
    window.setCentralWidget(new QWidget);
    QMenu* mesh = tearoff::enable(window.menuBar()->addMenu("Mesh"));
    mesh->addSection("Combine");
    mesh->addAction("Combine");
    mesh->addAction("Separate");
    mesh->addSeparator();
    mesh->addAction("Extract");
    QAction* normals = mesh->addSection("Normals");
    mesh->addAction("Soften Normals");
    window.show();
    settleBriefly();

    mesh->popup(QPoint(50, 50));
    settleBriefly();
    QPoint at = mesh->actionGeometry(normals).center();
    QMouseEvent press(QEvent::MouseButtonPress, at, mesh->mapToGlobal(at), Qt::LeftButton, Qt::LeftButton, Qt::NoModifier);
    QApplication::sendEvent(mesh, &press);
    QMouseEvent release(QEvent::MouseButtonRelease, at, mesh->mapToGlobal(at), Qt::LeftButton, Qt::NoButton, Qt::NoModifier);
    QApplication::sendEvent(mesh, &release);
    settleBriefly();
    QDockWidget* dock = window.findChild<QDockWidget*>("tearoff:Mesh/#Normals");
    REQUIRE(dock);
    CHECK(dock->windowTitle() == "Normals");
    CHECK(row(dock, "Soften Normals"));
    CHECK_FALSE(row(dock, "Combine"));

    QDockWidget* combine = tearoff::tearOff(mesh, QPoint(200, 200), "Combine");
    REQUIRE(combine);
    settleBriefly();
    CHECK(rows(combine).size() == 3);
    CHECK(row(combine, "Extract"));
    CHECK_FALSE(row(combine, "Soften Normals"));

    delete combine;
    tearoff::recreate(&window, {"tearoff:Mesh/#Combine"});
    QDockWidget* again = window.findChild<QDockWidget*>("tearoff:Mesh/#Combine");
    REQUIRE(again);
    CHECK(row(again, "Separate"));
}

TEST_CASE("a dragged Create row creates where it is dropped, as one undo step")
{
    lila::ops::registerBuiltins();
    lila::scene::types::registerBuiltins();
    lila::scene::registerCommands();
    actions::registerBuiltins();
    tearoff::registerDropHandler();
    lila::ops::execute("newScene", nlohmann::json::object());
    lila::scene::Graph& g = lila::scene::Graph::current();

    CHECK(tearoff::draggable("create.cube"));
    CHECK_FALSE(tearoff::draggable("edit.undo"));
    QMimeData mime;
    mime.setData(tearoff::kActionMime, "create.cube");
    CHECK(viewportHooks::handleDrop(mime, {}, false));

    const size_t before = g.nodes().size();
    std::vector<std::string> made = tearoff::runAt("create.cube", {100.0, 0.0, -50.0});
    REQUIRE(made.size() == 1);
    lila::scene::Node* cube = g.find(made[0]);
    REQUIRE(cube);
    const glm::dvec3 t = cube->as<glm::dvec3>(lila::scene::Transform::kTranslate);
    CHECK(t.x == doctest::Approx(100.0));
    CHECK(t.y == doctest::Approx(0.0));
    CHECK(t.z == doctest::Approx(-50.0));

    lila::ops::execute("undo", nlohmann::json::object());
    CHECK(g.nodes().size() == before);
    CHECK_FALSE(g.find(made[0]));
}

TEST_CASE("pressing a Create item and dragging out of its menu closes the menus")
{
    actions::registerBuiltins();
    QMainWindow window;
    window.setCentralWidget(new QWidget);
    window.resize(800, 600);
    QMenu* create = tearoff::enable(window.menuBar()->addMenu("Create"));
    QAction* locator = create->addAction("Locator");
    locator->setData(QString("create.locator"));
    create->addAction("Other");
    window.show();
    create->popup(window.mapToGlobal(QPoint(10, 30)));
    settleBriefly();
    REQUIRE(QApplication::activePopupWidget() == create);

    const QPoint on = create->actionGeometry(locator).center();
    QMouseEvent press(QEvent::MouseButtonPress, on, create->mapToGlobal(on), Qt::LeftButton, Qt::LeftButton, Qt::NoModifier);
    QApplication::sendEvent(create, &press);
    const QPoint out(create->width() + 200, on.y());
    QMouseEvent move(QEvent::MouseMove, out, create->mapToGlobal(out), Qt::NoButton, Qt::LeftButton, Qt::NoModifier);
    QApplication::sendEvent(create, &move);
    settleBriefly();
    CHECK(QApplication::activePopupWidget() == nullptr);
    CHECK_FALSE(create->isVisible());

    // The ghost carries the drag to the widget under the cursor and drops it there on release.
    QLabel* ghost = nullptr;
    for (QWidget* w : QApplication::topLevelWidgets())
        if (w->objectName() == "lilaMenuDrag")
            ghost = static_cast<QLabel*>(w);
    REQUIRE(ghost);
    DropCatcher catcher;
    catcher.setGeometry(300, 300, 200, 200);
    catcher.show();
    settleBriefly();
    const QPoint over = catcher.mapToGlobal(QPoint(100, 100));
    QMouseEvent hover(QEvent::MouseMove, ghost->mapFromGlobal(over), over, Qt::NoButton, Qt::LeftButton, Qt::NoModifier);
    QApplication::sendEvent(ghost, &hover);
    QMouseEvent release(QEvent::MouseButtonRelease, ghost->mapFromGlobal(over), over, Qt::LeftButton, Qt::NoButton, Qt::NoModifier);
    QApplication::sendEvent(ghost, &release);
    settleBriefly();
    CHECK(catcher.dropped == "create.locator");
}

TEST_CASE("a tear-off placed from the search follows the cursor until a click drops it")
{
    QApplication::setStyle(QStyleFactory::create("Fusion"));
    QMainWindow window;
    window.setCentralWidget(new QWidget);
    QMenu* lights = tearoff::enable(window.menuBar()->addMenu("Lights"));
    lights->addAction("Point Light");
    lights->addSection("Sky");
    lights->addAction("Sun");
    window.setGeometry(0, 0, 800, 600);
    window.show();
    settleBriefly();

    QCursor::setPos(200, 200);
    QDockWidget* dock = tearoff::tearOffToPlace(lights);
    REQUIRE(dock);
    settleBriefly();
    CHECK(dock->isFloating());
    CHECK(dock->property(tearoff::kPlacingProperty).toBool());

    QCursor::setPos(400, 300);
    for (int i = 0; i < 10 && dock->pos() != QPoint(380, 288); ++i)
    {
        wait(20);
        settleBriefly();
    }
    CHECK(dock->pos() == QPoint(380, 288));

    // The dropping click is the placer's: the row under it does not run.
    QPushButton* point = row(dock, "Point Light");
    REQUIRE(point);
    bool ran = false;
    QObject::connect(lights->actions().first(), &QAction::triggered, [&ran] { ran = true; });
    const QPoint at = point->rect().center();
    QMouseEvent press(QEvent::MouseButtonPress, at, point->mapToGlobal(at), Qt::LeftButton, Qt::LeftButton, Qt::NoModifier);
    QApplication::sendEvent(point, &press);
    QMouseEvent release(QEvent::MouseButtonRelease, at, point->mapToGlobal(at), Qt::LeftButton, Qt::NoButton, Qt::NoModifier);
    QApplication::sendEvent(point, &release);
    settleBriefly();
    CHECK_FALSE(ran);
    CHECK_FALSE(dock->property(tearoff::kPlacingProperty).toBool());
    QCursor::setPos(100, 100);
    wait(50);
    settleBriefly();
    CHECK(dock->pos() == QPoint(380, 288));

    // Esc puts back a section made for the placing.
    QDockWidget* sky = tearoff::tearOffToPlace(lights, "Sky");
    REQUIRE(sky);
    settleBriefly();
    QKeyEvent esc(QEvent::KeyPress, Qt::Key_Escape, Qt::NoModifier);
    QApplication::sendEvent(sky, &esc);
    settleBriefly();
    CHECK_FALSE(sky->isVisible());
}

TEST_CASE("menu columns: screen height wraps whole sections at dividers")
{
    lila::ui::applyTheme(*qApp);
    QWidget host;
    menus::MenuBuilder builder(&host, [](QMenu* menu, bool) { tearoff::enable(menu); });
    menus::addMenu({.id = "ColumnLayoutTest", .title = "Curves"});
    builder.rebuild();
    REQUIRE(builder.menu("ColumnLayoutTest"));
    QMenu& menu = *builder.menu("ColumnLayoutTest");
    const QRect screen = menu.screen()->availableGeometry();
    QAction* probe = menu.addAction("Probe");
    const int rowHeight = menu.actionGeometry(probe).height();
    REQUIRE(rowHeight > 0);
    menu.clear();
    const int count = std::max(2, int((screen.height() - 80) / (rowHeight * 2.4)));
    std::vector<std::vector<QAction*>> sections(3);
    int invoked = 0;
    for (int section = 0; section < 3; ++section)
    {
        menu.addSection(QString("Section %1").arg(section));
        for (int i = 0; i < count; ++i)
        {
            QAction* action = menu.addAction(QString("Item %1-%2").arg(section).arg(i));
            QObject::connect(action, &QAction::triggered, [&] { ++invoked; });
            sections[section].push_back(action);
        }
    }

    for (QPoint origin : {screen.topLeft() + QPoint(screen.width() / 3, 30), screen.bottomLeft() - QPoint(-10, 10)})
    {
        menu.popup(origin);
        settleBriefly();
        INFO("screen ", screen.x(), ",", screen.y(), " ", screen.width(), "x", screen.height(),
             "; menu ", menu.x(), ",", menu.y(), " ", menu.width(), "x", menu.height());
        CHECK(screen.contains(menu.geometry()));
        CHECK(menu.geometry().left() == origin.x());
        int contentBottom = 0;
        for (const auto& section : sections)
            contentBottom = std::max(contentBottom, menu.actionGeometry(section.back()).bottom() + 1);
        CHECK(menu.height() - contentBottom <= 10);
        CHECK(menu.actionGeometry(sections[2][0]).left() > menu.actionGeometry(sections[0][0]).left());
        for (const auto& section : sections)
        {
            const int column = menu.actionGeometry(section[0]).left();
            for (QAction* action : section)
            {
                const QRect rect = menu.actionGeometry(action);
                CHECK(rect.left() == column);
                CHECK(menu.rect().contains(rect));
            }
        }
        // Layout-only column padding must not appear as a button in a torn-off menu.
        tearoff::TearOffPanel panel(&menu);
        CHECK(panel.findChildren<QPushButton*>("tearOffRow").size() == 3 * count);
        menu.close();
    }

    // A larger font / smaller effective height must recalculate on the next opening.
    QFont font = menu.font();
    font.setPointSizeF(font.pointSizeF() * 1.2);
    menu.setFont(font);
    menu.popup(screen.topLeft() + QPoint(10, 10));
    settleBriefly();
    INFO("larger font menu ", menu.x(), ",", menu.y(), " ", menu.width(), "x", menu.height());
    CHECK(screen.contains(menu.geometry()));
    for (const auto& section : sections)
        CHECK(menu.actionGeometry(section.front()).left() == menu.actionGeometry(section.back()).left());

    if (const QString path = qEnvironmentVariable("LILA_TEST_GRAB"); !path.isEmpty())
        CHECK(menu.grab().save(path));

    // Keyboard navigation and invocation continue to use the original QActions across columns.
    menu.setActiveAction(sections[1].back());
    QKeyEvent down(QEvent::KeyPress, Qt::Key_Down, Qt::NoModifier);
    QApplication::sendEvent(&menu, &down);
    CHECK(menu.activeAction() == sections[2].front());
    QKeyEvent enter(QEvent::KeyPress, Qt::Key_Return, Qt::NoModifier);
    QApplication::sendEvent(&menu, &enter);
    CHECK(invoked == 1);
    menu.close();

    // A normal-height menu stays at its requested anchor after a tall layout was shown.
    menu.clear();
    menu.addAction("Undo\tCtrl+Z");
    menu.addAction("Redo\tCtrl+Shift+Z");
    const QPoint anchor = screen.topLeft() + QPoint(screen.width() / 3, 60);
    QMenu reference;
    reference.setFont(menu.font());
    reference.setTearOffEnabled(true);
    reference.addAction("Undo\tCtrl+Z");
    reference.addAction("Redo\tCtrl+Shift+Z");
    reference.popup(anchor);
    settleBriefly();
    const QRect expected = reference.geometry();
    reference.close();
    menu.popup(anchor);
    settleBriefly();
    CHECK(menu.geometry() == expected);
    CHECK(menu.actions().size() == 2);
    menu.close();

    // Open through an offset window's menu bar, as Edit is opened in the real application.
    host.setGeometry(screen.left() + screen.width() / 4, screen.top() + 50, screen.width() / 2, 300);
    QMenuBar bar(&host);
    bar.setGeometry(0, 0, host.width(), 26);
    menu.setTitle("Edit");
    bar.addMenu(&menu);
    host.show();
    bar.show();
    settleBriefly();
    const QRect button = bar.actionGeometry(menu.menuAction());
    const QPoint click = button.center();
    QMouseEvent press(QEvent::MouseButtonPress, click, bar.mapToGlobal(click), Qt::LeftButton, Qt::LeftButton, Qt::NoModifier);
    QApplication::sendEvent(&bar, &press);
    settleBriefly();
    CHECK(menu.isVisible());
    CHECK(std::abs(menu.geometry().left() - bar.mapToGlobal(button.topLeft()).x()) <= 10);
    CHECK(std::abs(menu.geometry().top() - bar.mapToGlobal(button.bottomLeft()).y()) <= 10);
    menu.close();
    menus::removeMenu("ColumnLayoutTest");
}

TEST_CASE("menu columns: floating window keeps every menu below its menu bar")
{
    lila::ui::applyTheme(*qApp);
    QWidget host;
    const QRect screen = host.screen()->availableGeometry();
    host.setGeometry(screen.left() + screen.width() / 4, screen.top() + 50, screen.width() / 2, 250);
    menus::MenuBuilder builder(&host, [](QMenu* menu, bool) { tearoff::enable(menu); });
    menus::addMenu({.id = "MenuFloorTest", .title = "Curves"});
    menus::addSubmenu("MenuFloorTest", "More");
    builder.rebuild();
    QMenu* menu = builder.menu("MenuFloorTest");
    QMenu* child = builder.menu("MenuFloorTest/More");
    REQUIRE(menu);
    REQUIRE(child);
    QMenuBar bar(&host);
    bar.setGeometry(0, 0, host.width(), 26);
    bar.addMenu(menu);
    QAction* probe = menu->addAction("Probe");
    const int rowHeight = menu->actionGeometry(probe).height();
    REQUIRE(rowHeight > 0);
    menu->removeAction(probe);
    delete probe;
    const int count = std::max(2, int((screen.height() - 80) / (rowHeight * 2.4)));
    for (int section = 0; section < 3; ++section)
    {
        menu->addSection(QString("Section %1").arg(section));
        for (int i = 0; i < count; ++i)
            menu->addAction(QString("Item %1-%2").arg(section).arg(i));
    }
    child->addSection("Long section");
    for (int i = 0; i < 3 * count; ++i)
        child->addAction(QString("Child %1").arg(i));
    host.show();
    bar.show();
    settleBriefly();

    for (int y : {screen.top() + 50, screen.top() + screen.height() / 3, screen.top() + screen.height() / 2})
    {
        host.move(host.x(), y);
        settleBriefly();
        const int floor = bar.mapToGlobal(QPoint(0, bar.height())).y();
        const QRect button = bar.actionGeometry(menu->menuAction());
        menu->popup(bar.mapToGlobal(button.bottomLeft()) + QPoint(0, 1));
        settleBriefly();
        INFO("floor ", floor, "; menu ", menu->geometry().y(), " height ", menu->height(), "; screen bottom ", screen.bottom());
        CHECK(menu->geometry().top() >= floor);
        CHECK(screen.contains(menu->geometry()));
        CHECK(menu->windowHandle()->transientParent() == host.windowHandle());
        CHECK(menu->windowType() == Qt::Popup);
        CHECK_FALSE(menu->windowFlags().testFlag(Qt::WindowStaysOnTopHint));
#if defined(Q_OS_LINUX) && QT_CONFIG(xcb)
        if (QGuiApplication::platformName() == "xcb")
        {
            auto* x11 = qApp->nativeInterface<QNativeInterface::QX11Application>();
            REQUIRE(x11);
            CHECK(menu->windowFlags().testFlag(Qt::BypassWindowManagerHint));
            auto* connection = x11->connection();
            auto* attributes = xcb_get_window_attributes_reply(connection, xcb_get_window_attributes(connection, menu->winId()), nullptr);
            REQUIRE(attributes);
            CHECK(attributes->override_redirect == 1);
            std::free(attributes);
        }
#endif
        int bottom = 0;
        for (QAction* action : menu->actions())
            if (!action->text().isEmpty())
            {
                CHECK(menu->rect().contains(menu->actionGeometry(action)));
                bottom = std::max(bottom, menu->actionGeometry(action).bottom() + 1);
            }
        CHECK(menu->height() - bottom <= 10);

        child->popup(menu->mapToGlobal(QPoint(menu->width(), menu->height() - 30)));
        settleBriefly();
        CHECK(child->geometry().top() >= floor);
        CHECK(screen.contains(child->geometry()));
        CHECK(child->windowHandle()->transientParent() == menu->windowHandle());
        for (QAction* action : child->actions())
            if (!action->text().isEmpty())
                CHECK(child->rect().contains(child->actionGeometry(action)));
        child->close();
        settleBriefly();
        if (y == screen.top() + screen.height() / 2 && QGuiApplication::platformName() == "xcb")
            if (const QString path = qEnvironmentVariable("LILA_TEST_GRAB"); !path.isEmpty())
                CHECK(menu->screen()->grabWindow(0).save(path));
        menu->close();
    }
    menus::removeMenu("MenuFloorTest");
}

TEST_CASE("floating shelf: grip drag moves the window and stops on release")
{
    QMainWindow host;
    host.show();
    if (FloatingShelf::active())
    {
        FloatingShelf::active()->close();
        QApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
    }
    FloatingShelf::toggle();
    QPointer<FloatingShelf> shelf = FloatingShelf::active();
    REQUIRE(shelf);
    settleBriefly();
    QLabel* grip = nullptr;
    for (QLabel* label : shelf->findChildren<QLabel*>())
        if (label->toolTip() == "Drag to move the shelf")
            grip = label;
    REQUIRE(grip);
    const QPoint before = shelf->pos();
    const QPoint local = grip->rect().center();
    const QPoint global = grip->mapToGlobal(local);
    const QPoint delta(45, 35);
    QMouseEvent press(QEvent::MouseButtonPress, local, global, Qt::LeftButton, Qt::LeftButton, Qt::NoModifier);
    QApplication::sendEvent(grip, &press);
    QMouseEvent move(QEvent::MouseMove, local + delta, global + delta, Qt::NoButton, Qt::LeftButton, Qt::NoModifier);
    QApplication::sendEvent(grip, &move);
    settleBriefly();
    CHECK(shelf->pos() == before + delta);
    QMouseEvent release(QEvent::MouseButtonRelease, local, global + delta, Qt::LeftButton, Qt::NoButton, Qt::NoModifier);
    QApplication::sendEvent(grip, &release);
    QMouseEvent hover(QEvent::MouseMove, local, global + 2 * delta, Qt::NoButton, Qt::NoButton, Qt::NoModifier);
    QApplication::sendEvent(grip, &hover);
    CHECK(shelf->pos() == before + delta);
    const QSize oldSize = shelf->size();
    const QPoint corner = shelf->rect().bottomRight() - QPoint(1, 1);
    const QPoint cornerGlobal = shelf->mapToGlobal(corner);
    const QPoint growth(30, 25);
    QMouseEvent resizePress(QEvent::MouseButtonPress, corner, cornerGlobal, Qt::LeftButton, Qt::LeftButton, Qt::NoModifier);
    QApplication::sendEvent(shelf, &resizePress);
    QMouseEvent resizeMove(QEvent::MouseMove, corner + growth, cornerGlobal + growth, Qt::NoButton, Qt::LeftButton, Qt::NoModifier);
    QApplication::sendEvent(shelf, &resizeMove);
    CHECK(shelf->size() == oldSize + QSize(growth.x(), growth.y()));
    QMouseEvent resizeRelease(QEvent::MouseButtonRelease, corner, cornerGlobal + growth, Qt::LeftButton, Qt::NoButton, Qt::NoModifier);
    QApplication::sendEvent(shelf, &resizeRelease);
    shelf->close();
    QApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
    CHECK(shelf.isNull());
}

TEST_CASE("floating tools: XCB tools bypass tiling without changing the main window")
{
    applyTheme(*qApp);
    QMainWindow host;
    host.show();
    QWidget tool(&host, Qt::Tool | Qt::FramelessWindowHint);
    tool.show();
    QDockWidget dock("Floating test", &host);
    dock.setTitleBarWidget(new DockTitleBar(&dock));
    host.addDockWidget(Qt::RightDockWidgetArea, &dock);
    dock.setFloating(true);
    dock.show();
    settleBriefly();
    const bool xcb = QGuiApplication::platformName() == "xcb";
    CHECK(tool.isVisible());
    CHECK(dock.isVisible());
    CHECK(tool.windowFlags().testFlag(Qt::BypassWindowManagerHint) == xcb);
    CHECK(dock.windowFlags().testFlag(Qt::BypassWindowManagerHint) == xcb);
    CHECK_FALSE(host.windowFlags().testFlag(Qt::BypassWindowManagerHint));
    REQUIRE(tool.windowHandle());
    CHECK(tool.windowHandle()->transientParent() == host.windowHandle());
    dock.setFloating(false);
    CHECK_FALSE(dock.isWindow());
    dock.setFloating(true);
    dock.show();
    settleBriefly();
    CHECK(dock.windowFlags().testFlag(Qt::BypassWindowManagerHint) == xcb);
}

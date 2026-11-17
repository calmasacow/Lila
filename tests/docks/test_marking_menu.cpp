#include <doctest/doctest.h>

#include "ui/Actions.h"
#include "ui/MarkingMenu.h"

#include <QApplication>
#include <QCursor>
#include <QKeyEvent>
#include <QMouseEvent>
#include <QPointer>

using namespace lila;

TEST_CASE("marking menu: viewport child, opaque ink only, gestures and dismissal")
{
    QWidget window;
    window.resize(1000, 800);
    QWidget viewport(&window);
    viewport.setGeometry(100, 100, 700, 600);
    viewport.setAttribute(Qt::WA_NativeWindow);
    viewport.setProperty("hotkeyContext", "viewport");
    window.show();
    QApplication::processEvents();

    int picked = 0;
    ui::actions::add({.id = "test.markingMenu", .label = "Pick", .run = [&] { ++picked; }, .doc = "Test marking menu dispatch."});
    const nlohmann::json def = {{"N", {{"label", "Pick"}, {"action", "test.markingMenu"}}}};
    const QPoint center = viewport.rect().center();
    auto open = [&]() {
        return ui::MarkingMenu::popupDefinition(def, ui::MarkingMenu::Trigger::MouseButton,
                                               Qt::RightButton, viewport.mapToGlobal(center));
    };
    QPointer<ui::MarkingMenu> menu = open();
    REQUIRE(menu);
    CHECK(menu->parentWidget() == &viewport);
    CHECK_FALSE(menu->isWindow());
    CHECK(menu->window() == &window);
    CHECK(menu->geometry() == viewport.rect());
    CHECK_FALSE(menu->testAttribute(Qt::WA_TranslucentBackground));
    CHECK(menu->graphicsEffect() == nullptr);
    menu->grab(); // paint the native child's shape, including its center and north item
    CHECK_FALSE(menu->mask().contains(QPoint(5, 5)));
    CHECK(menu->mask().contains(center));
    const QPoint north = center - QPoint(0, 110);
    CHECK(menu->mask().contains(north));

    QMouseEvent move(QEvent::MouseMove, north, menu->mapToGlobal(north), Qt::NoButton, Qt::RightButton, Qt::NoModifier);
    QApplication::sendEvent(menu, &move);
    QCursor::setPos(menu->mapToGlobal(north));
    QMouseEvent release(QEvent::MouseButtonRelease, north, menu->mapToGlobal(north), Qt::RightButton, Qt::NoButton, Qt::NoModifier);
    QApplication::sendEvent(menu, &release);
    QApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
    QApplication::processEvents();
    CHECK(picked == 1);
    CHECK(menu.isNull());
    CHECK(ui::MarkingMenu::active() == nullptr);

    menu = open();
    REQUIRE(menu);
    QKeyEvent escape(QEvent::KeyPress, Qt::Key_Escape, Qt::NoModifier);
    QApplication::sendEvent(menu, &escape);
    QApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
    CHECK(menu.isNull());
    CHECK(picked == 1);

    menu = open();
    REQUIRE(menu);
    viewport.resize(600, 500);
    QApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
    CHECK(menu.isNull());
    CHECK(ui::MarkingMenu::active() == nullptr);
    ui::actions::remove("test.markingMenu");
}

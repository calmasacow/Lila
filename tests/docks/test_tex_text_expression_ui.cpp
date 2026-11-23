#include <doctest/doctest.h>

#include "TestUtil.h"

#include "ops/Command.h"
#include "ui/FontPicker.h"
#include "ui/MultilineEdit.h"
#include "ui/TexParams.h"
#include "ui/TextureGraphUi.h"

#include <QAction>
#include <QLabel>
#include <QLineEdit>

using namespace lila;
using json = nlohmann::json;
using namespace dockstest;

namespace
{
json nodeInfo(const std::string& set, const std::string& name)
{
    const json info = run("texGraphInfo", {{"args", {set}}});
    for (const json& n : info["nodes"])
        if (n.value("name", "") == name)
        {
            json out = n;
            out["set"] = set;
            return out;
        }
    return json();
}

} // namespace

TEST_CASE("text node fields edit its words and font")
{
    registerTexGraphUi();
    run("newScene");
    const std::string set = run("texCreateSet", {{"args", {"Words"}}, {"resolution", 32}}).get<std::string>();
    const std::string text = run("texAddNode", {{"args", {set, "text"}}}).get<std::string>();
    std::map<std::string, json> sets;
    QWidget host;
    QWidget* panel = ui::texParams::nodePanel(&host, nodeInfo(set, text), [&](const std::string& id, const json& v) { sets[id] = v; });
    REQUIRE(panel);
    const QList<ui::MultilineEdit*> boxes = panel->findChildren<ui::MultilineEdit*>();
    REQUIRE(boxes.size() == 1);
    CHECK(boxes.front()->toPlainText() == "Lila");
    Q_EMIT boxes.front()->committed("Hello\nthere");
    CHECK(sets["text"] == "Hello\nthere");
    const QList<ui::FontPicker*> fonts = panel->findChildren<ui::FontPicker*>();
    REQUIRE(fonts.size() == 1);
    Q_EMIT fonts.front()->chosen("JetBrains Mono");
    CHECK(sets["font"] == "JetBrains Mono");
}


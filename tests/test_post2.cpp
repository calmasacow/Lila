#include <doctest/doctest.h>

#include "core/ImageIO.h"
#include "core/PrefStore.h"
#include "ops/Command.h"
#include "render/FrameState.h"
#include "render/Ocio.h"
#include "scene/ColorManagement.h"
#include "scene/Graph.h"
#include "ui/ViewportDisplay.h"

#include <filesystem>
#include <fstream>
#include <iterator>

using namespace lila;
using namespace lila::scene;
namespace fs = std::filesystem;

namespace
{
json post2Run(const char* name, json call = json::object()) { return ops::execute(name, call); }

fs::path post2Scratch(const char* name)
{
    const fs::path dir = fs::temp_directory_path() / "lila_post2_tests";
    fs::create_directories(dir);
    return dir / name;
}
} // namespace

TEST_CASE("the reflections pref reaches the frame")
{
    PrefStore& p = PrefStore::get();
    const std::string before = p.value<std::string>("general", ui::viewportDisplay::kReflections, "off");
    p.set("general", ui::viewportDisplay::kReflections, std::string("rayTraced"));
    p.set("general", ui::viewportDisplay::kReflectionCutoff, 0.4);
    render::FrameState s;
    ui::viewportDisplay::apply(s);
    CHECK(s.reflections == render::FrameState::Reflections::RayTraced);
    CHECK(s.reflectionCutoff == doctest::Approx(0.4f));
    p.set("general", ui::viewportDisplay::kReflections, std::string("screenSpace"));
    ui::viewportDisplay::apply(s);
    CHECK(s.reflections == render::FrameState::Reflections::ScreenSpace);
    p.set("general", ui::viewportDisplay::kReflections, before);
}

TEST_CASE("the built-in OCIO config lists displays and views and maps Lila's working space")
{
    const render::ocio::ConfigInfo info = render::ocio::info(render::ocio::kBuiltinConfig);
    REQUIRE(info.ok);
    CHECK(!info.displays.empty());
    CHECK(!info.views.empty());
    CHECK(!info.colorSpaces.empty());
    CHECK(info.workingSpace != "scene_linear");
}

TEST_CASE("an OCIO view bakes to a shaped LUT that rises with the light")
{
    std::string err;
    auto view = render::ocio::viewTransform("", "", "", "", &err);
    REQUIRE_MESSAGE(view, err);
    REQUIRE(view->lut);
    CHECK(view->lut->size == 65);
    std::vector<float> px = {0.0f, 0.0f, 0.0f, 1.0f, 0.18f, 0.18f, 0.18f, 1.0f, 1.0f, 1.0f, 1.0f, 1.0f, 16.0f, 16.0f, 16.0f, 1.0f};
    render::ocio::applyView(*view, px);
    CHECK(px[0] < 0.05f);
    CHECK(px[4] > px[0]);
    CHECK(px[8] > px[4]);
    CHECK(px[12] >= px[8]);
    CHECK(px[12] <= 1.05f);
    CHECK(px[3] == 1.0f);
    // The same request is the same bake.
    CHECK(render::ocio::viewTransform("", "", "", "") == view);
}

TEST_CASE("a broken OCIO config falls back to Lila's tonemapper and says why")
{
    post2Run("newScene");
    post2Run("colorManagement", {{"mode", "ocio"}, {"configPath", post2Scratch("missing.ocio").string()}});
    render::FrameState s;
    ui::viewportDisplay::applyColorManagement(s);
    CHECK(!s.post.view);
    CHECK(ui::viewportDisplay::colorNote().find("OCIO failed") != std::string::npos);
    post2Run("colorManagement", {{"configPath", ""}});
    ui::viewportDisplay::applyColorManagement(s);
    CHECK(s.post.view);
    CHECK(ui::viewportDisplay::colorNote().empty());
    post2Run("colorManagement", {{"mode", "lila"}});
    ui::viewportDisplay::applyColorManagement(s);
    CHECK(!s.post.view);
}

TEST_CASE("colour management settings save on their node and a pref can force the config")
{
    post2Run("newScene");
    const std::string node = post2Run("colorManagement", {{"mode", "ocio"}, {"display", "sRGB - Display"}}).get<std::string>();
    CHECK(post2Run("colorManagement").get<std::string>() == node);
    const colorManagement::Settings s = colorManagement::settings(Graph::current());
    CHECK(s.ocio);
    CHECK(s.display == "sRGB - Display");
    CHECK(s.config.empty());
    const colorManagement::Settings forced = colorManagement::settings(Graph::current(), "X:/studio/config.ocio");
    CHECK(forced.forced);
    CHECK(forced.config == "X:/studio/config.ocio");
}

TEST_CASE("multichannel EXRs write sorted layers with chromaticities and read back")
{
    const fs::path path = post2Scratch("layers.exr");
    std::vector<image::ExrChannel> channels;
    for (const char* name : {"R", "G", "B", "A", "depth.Z"})
        channels.push_back({name, std::vector<float>(4 * 2, name[0] == 'd' ? 5.0f : 0.5f), name[0] == 'd'});
    image::writeExrChannels(path, 4, 2, channels, {image::exrChromaticitiesRec709(), image::exrString("lila/test", "yes")});
    std::string err;
    const image::Image img = image::readExr(path, &err);
    REQUIRE_MESSAGE(img.width == 4, err);
    CHECK(img.rgba[0] == doctest::Approx(0.5f));
    std::ifstream f(path, std::ios::binary);
    const std::string bytes((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
    CHECK(bytes.find("depth.Z") != std::string::npos);
    CHECK(bytes.find("chromaticities") != std::string::npos);
    CHECK(bytes.find("lila/test") != std::string::npos);
}

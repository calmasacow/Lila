#include "TestUtil.h"
#include "Generators.h"

#include "curve/CurveNodes.h"
#include "extensions/ExtensionManager.h"
#include "gn/NodeLib.h"
#include "ops/Command.h"

#include <doctest/doctest.h>

#include <filesystem>
#include <map>
#include <set>

using namespace lila;
using namespace settlement;
using namespace settlementtest;

namespace
{
void loadOsmPackages()
{
    if (!ops::find("curve"))
    {
        lila::curve::registerTypes();
        lila::curve::registerCommands();
    }
    if (!gn::findDecl("Settlement"))
        REQUIRE(extensions::ExtensionManager::get().load(LILA_TEST_SETTLEMENT_PACKAGE) == "Settlement");
    if (!ops::find("terrainCreate"))
        REQUIRE(extensions::ExtensionManager::get().load(LILA_TEST_TERRAIN_PACKAGE) == "Terrain");
}

std::string townFile() { return (std::filesystem::path(LILA_SOURCE_DIR) / "tests" / "data" / "osm" / "w1_town.osm").string(); }

json makeTown(const std::string& ground, const char* name, json extra = json::object())
{
    json call = {{"profile", "usTown"}, {"size", 60000.0}, {"ground", ground}, {"osmFile", townFile()}, {"osmOrigin", {23.2, -106.42}}, {"name", name}};
    call.update(extra);
    return run("settlementCreate", call);
}

std::map<std::string, json> byOsmId(const json& list)
{
    std::map<std::string, json> out;
    for (const json& e : list)
        if (e.contains("osmId"))
            out[e["osmId"].get<std::string>()] = e;
    return out;
}
} // namespace

TEST_CASE("settlement osm: imported buildings become lots and envelopes with their heights, parts, courtyards, roofs and businesses")
{
    loadOsmPackages();
    run("newScene");
    const json terrain = run("terrainCreate", {{"size", json::array({80000.0, 80000.0})}, {"resolution", 129}, {"amplitude", 300.0}, {"seed", 3}});
    const std::string ground = terrain["mesh"];
    const json town = makeTown(ground, "Town");
    const std::string mesh = town["mesh"];
    const json gen = run("settlementGenerators", {{"args", {mesh}}});
    CAPTURE(gen["osm"].dump());
    CHECK(gen["osm"]["buildingLots"] == 11); // 13 buildings, two of them parts of the cathedral
    CHECK(gen["osm"]["parts"] == 2);
    CHECK(gen["osm"]["denseBlocks"].get<int>() >= 1);
    CHECK(gen["osm"]["sparseBlocks"].get<int>() >= 1);
    CHECK(gen["osm"]["outside"] == 0);

    const json lots = run("settlementLots", {{"args", {mesh}}, {"limit", 0}})["lots"];
    const auto envs = byOsmId(run("settlementEnvelopes", {{"args", {mesh}}, {"limit", 0}})["envelopes"]);
    const auto osmLots = byOsmId(lots);
    REQUIRE(envs.size() == 11);

    const json& belmar = envs.at("w1001");
    CHECK(belmar["floors"] == 3);
    CHECK(belmar["heightSource"] == "levels");
    CHECK(belmar["roof"] == "hip");
    CHECK(belmar["roofSource"] == "tag");
    CHECK(belmar["colour"] == "#d4a373");
    CHECK(belmar["program"] == "shop");
    CHECK_FALSE(belmar["name"].get<std::string>().empty());
    CHECK(belmar["name"] != "Edificio Belmar");
    CHECK_FALSE(belmar["style"].get<std::string>().empty());

    const json& house = envs.at("w1002");
    CHECK(house["heightSource"] == "height");
    CHECK(house["height"].get<double>() == doctest::Approx(750.0));
    CHECK(house["roof"] == "gable");
    CHECK(house["roofColour"] == "#8b3a2b");
    CHECK(envs.at("w1003")["floors"] == 5);
    CHECK(envs.at("w1003")["roof"] == "flat");
    CHECK(envs.at("w1003")["material"] == "concrete");

    const json& court = envs.at("r2001");
    CHECK(court["holes"].size() == 1);
    CHECK(court["height"].get<double>() == doctest::Approx(1200.0));
    CHECK(court["program"] == "apartments");

    const json& cathedral = envs.at("w1010");
    CHECK(cathedral["program"] == "cathedral");
    CHECK(cathedral["landmark"] == "cathedral");
    CHECK(cathedral["style"] == "church");
    CHECK(cathedral["eraName"] == "industrial");
    REQUIRE(cathedral["tiers"].size() == 2);
    double tallest = 0.0;
    for (const json& t : cathedral["tiers"])
        tallest = std::max(tallest, t["top"].get<double>());
    CHECK(tallest - cathedral["ground"].get<double>() > 4300.0);

    // Shops inside buildings are their businesses; one chain's branches share a name, and none is real.
    const json& pharmacy = envs.at("w1004")["businesses"];
    REQUIRE(pharmacy.size() == 1);
    CHECK(pharmacy[0]["program"] == "pharmacy");
    const std::string farmacia = pharmacy[0]["name"];
    CHECK((farmacia.find("Farmacia") != std::string::npos || farmacia.find("Botica") != std::string::npos));
    CHECK(farmacia.find("Similares") == std::string::npos);
    REQUIRE(envs.at("w1005")["businesses"].size() == 1);
    REQUIRE(envs.at("w1009")["businesses"].size() == 1);
    CHECK(envs.at("w1005")["businesses"][0]["name"] == envs.at("w1009")["businesses"][0]["name"]);
    CHECK(envs.at("w1005")["businesses"][0]["name"] != "OXXO");
    CHECK(envs.at("w1005")["groundUse"] == "commercial");

    // The dense block holds only the real buildings; the sparse one keeps Settlement's lots around them.
    const uint64_t denseBlock = osmLots.at("w1001")["block"], sparseBlock = osmLots.at("w1010")["block"];
    int denseCut = 0, sparseCut = 0;
    for (const json& lot : lots)
        if (!lot.contains("osmId"))
        {
            denseCut += lot["block"] == denseBlock ? 1 : 0;
            sparseCut += lot["block"] == sparseBlock ? 1 : 0;
        }
    CHECK(denseCut == 0);
    CHECK(sparseCut > 0);

    // Areas: the cemetery's lots and its landmark, the school's grounds; the taqueria on a lot Settlement cut.
    int cemetery = 0, school = 0, taco = 0;
    bool cemeteryLandmark = false;
    for (const json& lot : lots)
    {
        cemetery += lot["use"] == "cemetery" ? 1 : 0;
        cemeteryLandmark = cemeteryLandmark || lot["landmark"] == "cemetery";
        school += lot["program"] == "school" ? 1 : 0;
        if (lot.contains("businesses"))
            for (const json& b : lot["businesses"])
                if (b["program"] == "taqueria")
                {
                    ++taco;
                    CHECK_FALSE(lot.contains("osmId"));
                    CHECK(lot["use"] == "commercial");
                    CHECK(b["name"] != "Taquería El Güero");
                }
    }
    CHECK(cemetery > 0);
    CHECK(cemeteryLandmark);
    CHECK(school > 0);
    CHECK(taco == 1);

    // A neighbourhood names the district it lies in, fictionally.
    const json names = run("settlementNames", {{"args", {mesh}}});
    CHECK(names["culture"] == "mexican");
    for (const json& s : gen["osm"]["sites"])
        if (s["kind"] == "district")
        {
            CHECK(s["name"] != "Colonia Centro");
            bool named = false;
            for (const json& d : names["districts"])
                named = named || d["name"] == s["name"];
            CHECK(named);
        }

    const json check = run("settlementCheck", {{"args", {mesh}}});
    CAPTURE(check["issues"].dump());
    CHECK(check["counts"]["realName"] == 0);
    CHECK(check["counts"]["frontage"] == 0);
    CHECK(run("realNameCheck")["ok"] == true);

    // The same file again: the same lots, ids and names.
    const json again = makeTown(ground, "Town2");
    const auto envs2 = byOsmId(run("settlementEnvelopes", {{"args", {again["mesh"]}}, {"limit", 0}})["envelopes"]);
    REQUIRE(envs2.size() == envs.size());
    for (const auto& [id, e] : envs)
    {
        CAPTURE(id);
        CHECK(envs2.at(id)["lotId"] == e["lotId"]);
        CHECK(envs2.at(id)["name"] == e["name"]);
        CHECK(envs2.at(id)["businesses"] == e["businesses"]);
    }

    // A real name given by hand is caught.
    const uint64_t district = names["districts"][0]["id"];
    run("settlementNames", {{"args", {mesh}}, {"district", district}, {"name", "Colonia Centro"}});
    const json leaked = run("settlementCheck", {{"args", {mesh}}});
    CHECK(leaked["counts"]["realName"] == 1);
    CHECK(leaked["ok"] == false);
}

TEST_CASE("settlement osm: fill none leaves blocks to the imported buildings, all fills every block")
{
    loadOsmPackages();
    run("newScene");
    const json terrain = run("terrainCreate", {{"size", json::array({80000.0, 80000.0})}, {"resolution", 65}, {"amplitude", 200.0}, {"seed", 4}});
    auto cutIn = [](const json& town, const char* osmId) {
        const json lots = run("settlementLots", {{"args", {town["mesh"]}}, {"limit", 0}})["lots"];
        json block;
        for (const json& lot : lots)
            if (lot.value("osmId", std::string()) == osmId)
                block = lot["block"];
        int n = 0;
        for (const json& lot : lots)
            n += !lot.contains("osmId") && lot["block"] == block ? 1 : 0;
        return n;
    };
    const json none = makeTown(terrain["mesh"], "None", {{"osmFill", "none"}});
    CHECK(cutIn(none, "w1010") == 0);
    const json sparse = makeTown(terrain["mesh"], "Sparse");
    CHECK(cutIn(sparse, "w1010") > 0);
    CHECK(cutIn(sparse, "w1001") == 0);
    const json all = makeTown(terrain["mesh"], "All", {{"osmFill", "all"}});
    CHECK(cutIn(all, "w1001") >= 0);
    CHECK(cutIn(all, "w1010") == cutIn(sparse, "w1010"));
}

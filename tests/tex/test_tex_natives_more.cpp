#include "TexTest.h"

#include "tex/Natives.h"

#include <set>

using namespace textest;

namespace
{
image::Image at64(const Graph& g, const std::string& from)
{
    return eval(into(g, from), "Output", "baseColor", 64);
}

/** A constant value through type, read back. */
float constantThrough(const std::string& type, double input, json values)
{
    Graph g;
    add(g, "C", "constant", {{"value", input}});
    add(g, "N", type, std::move(values));
    link(g, "C.out", "N.input");
    return texel(eval(into(g, "N.out"), "Output", "baseColor", 4), 1, 1)[0];
}

/** A pixel processor writing expression's colour. */
Graph ramp(const std::string& expression)
{
    Graph g;
    add(g, "Src", "pixelProcessor", {{"expression", expression}});
    return g;
}

/** Bricks (2 columns, 4 rows, no offset) through a flood fill into type. */
Graph bricksInto(const std::string& type, json values = json::object())
{
    Graph g;
    add(g, "B", "bricks", {{"rows", 4}, {"columns", 2}, {"offset", 0.0}, {"bevel", 0.01}, {"mortar", 0.15}});
    add(g, "F", "floodFill");
    add(g, "N", type, std::move(values));
    link(g, "B.mask", "F.input");
    link(g, "F.out", "N.input");
    return g;
}

/** The node's CPU result against its Slang kernels run through the CPU target, the way the GPU runs them. */
double kernelGap(const std::string& type, const Graph& g, const std::string& from)
{
    Plan p = plan(into(g, from), "Output", "baseColor", 48);
    REQUIRE_MESSAGE(p.error.empty(), p.error);
    image::Image a = evaluateCpu(p).outputs.at("out");
    NativeCpuFn cpu = nativeCpu(type);
    REQUIRE(cpu);
    registerNativeCpu(type, [](const NativeCall& call) { runNativeKernelsCpu(call); });
    image::Image k = evaluateCpu(p).outputs.at("out");
    registerNativeCpu(type, cpu);
    return maxDiff(a, k);
}
} // namespace

TEST_CASE("tex natives more: histogram shift wraps")
{
    CHECK(constantThrough("histogramShift", 0.3, {{"position", 0.5}}) == doctest::Approx(0.8));
    CHECK(constantThrough("histogramShift", 0.3, {{"position", 0.8}}) == doctest::Approx(0.1));
    CHECK(constantThrough("histogramShift", 1.0, {{"position", 0.0}}) == doctest::Approx(1.0));
}

TEST_CASE("tex natives more: histogram select keeps a band")
{
    auto value = constantThrough;
    CHECK(value("histogramSelect", 0.5, json::object()) == doctest::Approx(1.0));
    CHECK(value("histogramSelect", 0.9, json::object()) == doctest::Approx(0.0));
    CHECK(value("histogramSelect", 0.55, {{"contrast", 0.0}}) == doctest::Approx(0.6));
    CHECK(value("histogramSelect", 0.6, {{"contrast", 1.0}}) == doctest::Approx(1.0));
}

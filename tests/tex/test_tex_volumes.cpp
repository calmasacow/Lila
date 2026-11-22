#include <doctest/doctest.h>

#include "render/Atmosphere.h"
#include "render/FrameState.h"
#include "render/Renderer.h"

#include <glm/gtc/matrix_transform.hpp>

#include <cmath>
#include <string>

using namespace lila::render;

namespace pathtest
{
bool tracing();
std::string uniformHdr(float value);
}

namespace
{
constexpr int kSize = 64;

/** Looking at the origin from +Z with nothing but the given volumes, under a uniform environment of radiance 1. */
FrameState volumeFrame(bool pathTraced)
{
    FrameState s;
    s.view = glm::lookAt(glm::dvec3(0, 0, 300), glm::dvec3(0), glm::dvec3(0, 1, 0));
    const double f = 1.0 / std::tan(glm::radians(20.0));
    glm::dmat4 p(0.0);
    p[0][0] = f;
    p[1][1] = f;
    p[2][3] = -1.0;
    p[3][2] = 1.0;
    s.proj = p;
    s.eye = glm::dvec3(0, 0, 300);
    s.grid = false;
    s.envPath = pathtest::uniformHdr(1.0f);
    s.envBackground = true;
    s.envBackgroundExposure = 1.0f;
    s.envLightSaturation = 1.0f;
    s.groundShadow = 0.0f;
    s.ambientOcclusion = false;
    s.msaaSamples = 1;
    s.taa = false;
    s.post.tonemapper = PostSettings::Linear;
    s.pathTrace.on = pathTraced;
    s.pathTrace.bounces = 16;
    s.pathTrace.samplesPerFrame = 32;
    s.pathTrace.maxSamples = 4096;
    s.pathTrace.denoise = false;
    s.pathTrace.seed = 5;
    return s;
}

/** A cube of value 1, 8 voxels of 10 cm a side, centred on the origin. */
VolumeDraw cube()
{
    VolumeDraw v;
    v.dims = glm::ivec3(8);
    v.values = std::make_shared<std::vector<float>>(512, 1.0f);
    v.indexToWorld = glm::translate(glm::dmat4(1.0), glm::dvec3(-35.0)) * glm::scale(glm::dmat4(1.0), glm::dvec3(10.0));
    v.peak = 1.0f;
    return v;
}

std::vector<float> renderVolumes(const FrameState& s, int frames)
{
    Renderer::OffscreenResult out;
    std::string err;
    REQUIRE_MESSAGE(Renderer::get().renderOffscreenFrames({s}, kSize, kSize, frames, true, out, &err), err);
    REQUIRE(out.hdr.size() == size_t(kSize) * kSize * 4);
    return out.hdr;
}

/** Mean of the middle 8 x 8 pixels, all channels. */
double centre(const std::vector<float>& hdr)
{
    double sum = 0.0;
    for (int y = kSize / 2 - 4; y < kSize / 2 + 4; ++y)
        for (int x = kSize / 2 - 4; x < kSize / 2 + 4; ++x)
            for (int c = 0; c < 3; ++c)
                sum += hdr[(size_t(y) * kSize + x) * 4 + c];
    return sum / (8 * 8 * 3);
}

/** Through the cube's middle: 70 cm at full density, then trilinear falloff to half over the 5 cm either side. */
constexpr double kOpticalLength = 70.0 + 2.0 * 5.0 * 0.75;
} // namespace

TEST_CASE("volumes: a pure absorber lets through exp(-optical depth), ray marched and path traced")
{
    if (!pathtest::tracing())
        return;
    for (bool traced : {false, true})
    {
        CAPTURE(traced);
        FrameState s = volumeFrame(traced);
        VolumeDraw v = cube();
        v.density = 0.01f;
        v.scatter = glm::vec3(0.0f);
        v.absorption = glm::vec3(1.0f);
        s.volumes.push_back(v);
        const double expected = std::exp(-0.01 * kOpticalLength);
        CHECK(centre(renderVolumes(s, traced ? 16 : 1)) == doctest::Approx(expected).epsilon(0.06));
    }
}

TEST_CASE("volumes: furnace, a white scattering volume under a uniform environment matches the environment")
{
    if (!pathtest::tracing())
        return;
    for (bool traced : {false, true})
    {
        CAPTURE(traced);
        FrameState s = volumeFrame(traced);
        VolumeDraw v = cube();
        v.density = 0.03f;
        v.scatter = glm::vec3(1.0f);
        v.absorption = glm::vec3(0.0f);
        s.volumes.push_back(v);
        CHECK(centre(renderVolumes(s, traced ? 32 : 1)) == doctest::Approx(1.0).epsilon(0.05));
    }
}

TEST_CASE("volumes: emission adds light where there is density")
{
    if (!pathtest::tracing())
        return;
    FrameState s = volumeFrame(false);
    VolumeDraw v = cube();
    v.density = 0.001f;
    v.scatter = glm::vec3(0.0f);
    v.absorption = glm::vec3(1.0f);
    v.emission = glm::vec3(10.0f, 0.0f, 0.0f);
    s.volumes.push_back(v);
    const std::vector<float> hdr = renderVolumes(s, 1);
    const float* mid = &hdr[(size_t(kSize / 2) * kSize + kSize / 2) * 4];
    CHECK(mid[0] > mid[2] + 0.3f);
    // Outside the cube only the environment shows.
    CHECK(hdr[0] == doctest::Approx(1.0f).epsilon(0.02));
}

TEST_CASE("atmosphere: the path tracer and the viewport draw the same sky behind")
{
    if (!pathtest::tracing())
        return;
    std::vector<double> blueOverRed;
    for (bool traced : {false, true})
    {
        FrameState s = volumeFrame(traced);
        AtmosphereFrame a;
        a.sunDir = glm::normalize(glm::dvec3(0.3, 0.6, -0.7));
        a.sunRadiance = glm::vec3(3.0f);
        s.atmosphere = a;
        s.envPath = atmosphereEnvironmentKey(a, s.eye);
        const std::vector<float> hdr = renderVolumes(s, traced ? 4 : 1);
        const float* mid = &hdr[(size_t(kSize / 2) * kSize + kSize / 2) * 4];
        CHECK(mid[2] > mid[0]);
        blueOverRed.push_back(double(mid[2]) / std::max(double(mid[0]), 1e-6));
    }
    CHECK(blueOverRed[1] == doctest::Approx(blueOverRed[0]).epsilon(0.05));
}

#include <doctest/doctest.h>

#include "ops/Command.h"
#include "ui/PerfTrace.h"

using namespace lila;

TEST_CASE("performance traces: the dialog records up to two minutes, commands up to three hours")
{
    using ui::perfTrace::recordingSeconds;
    CHECK(recordingSeconds(10, false) == 10);
    CHECK(recordingSeconds(600, false) == 120);
    CHECK(recordingSeconds(0, false) == 1);
    CHECK(recordingSeconds(600, true) == 600);
    CHECK(recordingSeconds(3 * 3600, true) == 3 * 3600);
    CHECK(recordingSeconds(3 * 3600 + 1, true) == 3 * 3600);
    CHECK(recordingSeconds(-5, true) == 1);

    ops::registerBuiltins();
    if (!ops::find("perfTraceRecord"))
        ui::perfTrace::registerAll(nullptr);
    const CommandDef* record = ops::find("perfTraceRecord");
    REQUIRE(record);
    REQUIRE(!record->flags.empty());
    CHECK(record->flags[0].name == "seconds");
    REQUIRE(record->flags[0].max);
    CHECK(*record->flags[0].max == doctest::Approx(3 * 3600));
}

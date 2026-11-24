// Naming at world scale: one NameBook handing out hundreds of names of one program stays fast and unique.

#include "names/Names.h"

#include <doctest/doctest.h>

#include <chrono>
#include <set>

using namespace lila::names;

TEST_CASE("names: 2000 unnamed shops from one book are unique, fictional and fast")
{
    for (const char* culture : {"mexican", "oldEnglish"})
    {
        NameBook book(culture);
        std::set<std::string> seen;
        const auto t0 = std::chrono::steady_clock::now();
        for (uint64_t k = 0; k < 2000; ++k)
        {
            const std::string n = book.place(Kind::Business, "shop", 0x1000 + k);
            CHECK_FALSE(n.empty());
            CHECK(seen.insert(n).second);
            CHECK_FALSE(book.real(n));
        }
        const double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
        MESSAGE(culture << ": 2000 shop names in " << ms << " ms");
        CHECK(ms < 3000.0);
        // Asking again gives the same names without new work.
        CHECK(book.place(Kind::Business, "shop", 0x1000) == book.place(Kind::Business, "shop", 0x1000));
    }
}

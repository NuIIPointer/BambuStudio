#include <catch2/catch.hpp>

#include "libslic3r/PrintConfig.hpp"
#include "libslic3r/Shading.hpp"

using namespace Slic3r;
using namespace Slic3r::Shading;

static DynamicPrintConfig shading_config(const std::vector<std::string> &colors)
{
    DynamicPrintConfig config = DynamicPrintConfig::full_print_config();
    config.set_key_value("filament_colour", new ConfigOptionStrings(colors));
    config.set_key_value("filament_is_mixed", new ConfigOptionBools(std::vector<unsigned char>(colors.size(), 0)));
    return config;
}

TEST_CASE("Shade states map to signed levels and back", "[Shading]")
{
    CHECK(state_to_level(0) == 0);
    CHECK(state_to_level(1) == -1);
    CHECK(state_to_level(kLevels) == -kLevels);
    CHECK(state_to_level(kLevels + 1) == 1);
    CHECK(state_to_level(2 * kLevels) == kLevels);
    for (int level = -kLevels; level <= kLevels; ++level)
        CHECK(state_to_level(level_to_state(level)) == level);
}

TEST_CASE("Shading filaments are picked by brightness", "[Shading]")
{
    SECTION("automatic") {
        ShadeSettings s = ShadeSettings::from_config(shading_config({ "#C12E1F", "#FFFFFF", "#000000" }));
        CHECK(s.light_filament == 1);
        CHECK(s.dark_filament == 2);
    }
    SECTION("no light or dark filament available") {
        ShadeSettings s = ShadeSettings::from_config(shading_config({ "#C12E1F", "#2E6BC1" }));
        CHECK(s.light_filament == -1);
        CHECK(s.dark_filament == -1);
    }
    SECTION("explicit choice wins") {
        DynamicPrintConfig config = shading_config({ "#FFFFFF", "#F0F0F0", "#000000" });
        config.set_key_value("shading_light_filament", new ConfigOptionInt(2));
        ShadeSettings s = ShadeSettings::from_config(config);
        CHECK(s.light_filament == 1);
    }
}

TEST_CASE("Layer alternation spreads the shade evenly over the height", "[Shading]")
{
    ShadeSettings s;
    s.light_filament = 1;
    s.dark_filament  = 2;
    const double lh  = 0.2;

    for (int level : { 4, 8, 16 }) {
        ShadeTag tag{ int8_t(level), false };
        int      shaded = 0;
        for (size_t layer = 1; layer <= 160; ++layer) {
            ShadePlan plan = plan_shade(s, tag, layer, layer * lh, lh, 0);
            if (plan.active()) {
                CHECK(plan.full(lh));
                CHECK(plan.filament == 1);
                ++shaded;
            }
        }
        CHECK(shaded == Approx(160. * level / kLevels).margin(1.));
    }

    // Darker shades use the dark filament, the first layer and the main filament itself are never shaded.
    CHECK(plan_shade(s, { int8_t(-16), false }, 5, 1., lh, 0).filament == 2);
    CHECK_FALSE(plan_shade(s, { int8_t(16), false }, 0, lh, lh, 0).active());
    CHECK_FALSE(plan_shade(s, { int8_t(16), false }, 5, 1., lh, 1).active());
}

TEST_CASE("Sublayer thickness follows the shade with carried over rounding", "[Shading]")
{
    ShadeSettings s;
    s.light_filament = 1;
    s.sublayer       = true;
    s.min_sublayer   = 0.04;
    const double lh  = 0.2;

    for (int level : { 2, 5, 11, 16 }) {
        const double density = double(level) / kLevels;
        double       total   = 0.;
        for (size_t layer = 1; layer <= 100; ++layer) {
            ShadePlan plan = plan_shade(s, { int8_t(level), false }, layer, (layer + 1) * lh, lh, 0);
            if (!plan.active())
                continue;
            CHECK(plan.height >= s.min_sublayer - EPSILON);
            CHECK(plan.height <= lh + EPSILON);
            // The main color part is either gone or not thinner than the minimum.
            CHECK((plan.full(lh) || lh - plan.height >= 0.5 * s.min_sublayer));
            total += plan.height;
        }
        CHECK(total == Approx(density * 100 * lh).margin(2 * s.min_sublayer));
    }
}

TEST_CASE("Outer wall tint only exists with sublayers", "[Shading]")
{
    ShadeSettings s;
    s.light_filament = 1;
    s.outer_ratio    = 0.5;
    ShadeTag outer{ int8_t(16), true };
    CHECK_FALSE(plan_shade(s, outer, 3, 0.8, 0.2, 0).active());
    s.sublayer = true;
    double total = 0.;
    for (size_t layer = 1; layer <= 50; ++layer)
        total += plan_shade(s, outer, layer, (layer + 1) * 0.2, 0.2, 0).height;
    CHECK(total == Approx(0.5 * 50 * 0.2).margin(0.1));
}

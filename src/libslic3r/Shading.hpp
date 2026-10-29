#ifndef slic3r_Shading_hpp_
#define slic3r_Shading_hpp_

// Airbrush shading: the outer wall keeps the main color, the inner walls right behind it are printed
// in a white or black filament on some layers (or in a part of a layer) so that the shade shimmers
// through the thin outer wall.

#include <algorithm>
#include <cstdint>
#include <memory>
#include <vector>

#include "ExPolygon.hpp"
#include "Point.hpp"

namespace Slic3r {

class ConfigBase;
class ExtrusionEntityCollection;
class Layer;
class ModelObject;
class PrintObject;

namespace Shading {

// Shade levels per direction. Painted states 1..kLevels are darker, kLevels+1..2*kLevels are lighter.
constexpr int kLevels = 16;

// Painted triangle state -> signed level in [-kLevels, kLevels], 0 = unchanged.
inline int state_to_level(int state)
{
    if (state <= 0 || state > 2 * kLevels)
        return 0;
    return state <= kLevels ? -state : state - kLevels;
}

// Signed level -> painted triangle state.
inline int level_to_state(int level)
{
    if (level == 0)
        return 0;
    if (level < 0)
        return std::min(-level, kLevels);
    return kLevels + std::min(level, kLevels);
}

// Tag of a perimeter or fill collection that holds shaded pieces.
struct ShadeTag
{
    int8_t level = 0;     // signed shade level, 0 = not a shade collection
    bool   outer = false; // piece of the outer wall (only with outer wall tint)
    bool   skin  = false; // solid infill right below a top or right above a bottom surface

    bool empty() const { return level == 0; }
};

// Shading settings of one object, the shading filaments resolved to physical filaments.
struct ShadeSettings
{
    int    light_filament = -1; // 0-based, -1 = none available
    int    dark_filament  = -1; // 0-based, -1 = none available
    bool   sublayer       = false;
    int    wall_depth     = 1;
    double strength       = 1.;
    double min_sublayer   = 0.04;
    double outer_ratio    = 0.;

    // The shading filaments come from the print config, the other settings from the object config.
    static ShadeSettings from_config(const ConfigBase &print_config, const ConfigBase &object_config);
    // Both from one full config, e.g. the full DynamicPrintConfig of the GUI.
    static ShadeSettings from_config(const ConfigBase &config) { return from_config(config, config); }
    int filament_for(int level) const { return level < 0 ? dark_filament : light_filament; }
};

// What to print for one shade collection on one layer.
struct ShadePlan
{
    int    filament = -1; // 0-based shade filament, -1 = print the whole piece in the main color
    double height   = 0.; // height of the shade part; equals the layer height when the whole piece is shaded

    bool active() const { return filament >= 0 && height > 0.; }
    bool full(double layer_height) const { return active() && height >= layer_height - EPSILON; }
};

// 0-based shading filaments the airbrush painting of the object needs (lighter and/or darker).
std::vector<unsigned int> used_shading_filaments(const ModelObject &object, const ShadeSettings &settings);

// Plan a shade collection of a layer. main_filament is the 0-based filament the wall would use without shading.
ShadePlan plan_shade(const ShadeSettings &settings, const ShadeTag &tag, size_t layer_id, double print_z, double layer_height, int main_filament);

// Plan a shaded solid infill collection below a top or above a bottom surface. The skin is a single layer,
// so the shade is always built from the thickness of a sublayer, in both shading modes.
ShadePlan plan_skin_shade(const ShadeSettings &settings, const ShadeTag &tag, size_t layer_id, double layer_height, int main_filament);

// Lookup of the painted shade level of the closest side surface of an object.
class ShadeField
{
public:
    // Returns nullptr when the object has no airbrush painting.
    static std::unique_ptr<ShadeField> build(const PrintObject &object);
    ~ShadeField();

    // Shade level of the side surface closest to point (unscaled, object coordinates as the slices),
    // 0 when there is no side surface closer than max_dist.
    int level_at(const Vec3d &point, double max_dist) const;

private:
    ShadeField();
    struct Impl;
    std::unique_ptr<Impl> m_impl;
};

// The painted shade of the top and bottom surfaces of an object, projected onto the XY plane.
class SkinShadeField
{
public:
    // Returns nullptr when no top or bottom surface of the object is painted.
    static std::unique_ptr<SkinShadeField> build(const PrintObject &object);
    ~SkinShadeField();

    // Painted areas (scaled, object coordinates as the slices) per signed level of the upward (top) or downward
    // (bottom) facing surfaces between z_min and z_max.
    std::vector<std::pair<int, ExPolygons>> areas(bool top, double z_min, double z_max) const;

private:
    SkinShadeField();
    struct Impl;
    std::unique_ptr<Impl> m_impl;
};

// Cut the solid infill right below a painted top surface and right above a painted bottom surface into shade
// pieces, stored as tagged collections in LayerRegion::fills. Called right after the fills of the layer were
// generated, the slices of the neighbor layers must be final.
void split_shaded_skins(Layer &layer, const SkinShadeField &field);

// Cut the shaded parts out of the walls of a layer and store them as tagged island collections at the front
// of LayerRegion::perimeters. Called right after the perimeters of the layer were generated.
void split_shaded_walls(Layer &layer, const ShadeField &field, const ShadeSettings &settings);

} // namespace Shading
} // namespace Slic3r

#endif // slic3r_Shading_hpp_

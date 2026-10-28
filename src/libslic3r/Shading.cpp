#include "Shading.hpp"

#include <cmath>
#include <map>

#include "AABBTreeIndirect.hpp"
#include "Color.hpp"
#include "ExtrusionEntity.hpp"
#include "ExtrusionEntityCollection.hpp"
#include "Layer.hpp"
#include "Model.hpp"
#include "Print.hpp"
#include "PrintConfig.hpp"
#include "TriangleSelector.hpp"

namespace Slic3r {
namespace Shading {

namespace {

double filament_luminance(const std::string &hex)
{
    ColorRGB color;
    if (!decode_color(hex, color))
        return -1.;
    return 0.2126 * color.r() + 0.7152 * color.g() + 0.0722 * color.b();
}

// Side surfaces only: closer to horizontal faces would make the top and bottom skins hide the side painting.
constexpr float kMaxSideNormalZ = 0.9f;
// Sampling step along the walls and the shortest run of one level that is kept.
constexpr double kSampleStep  = 0.3;
constexpr double kMinRunLength = 1.0;

} // namespace

ShadeSettings ShadeSettings::from_config(const ConfigBase &config)
{
    ShadeSettings s;
    auto get_int = [&config](const char *key, int def) {
        const ConfigOption *opt = config.option(key);
        return opt != nullptr ? opt->getInt() : def;
    };
    auto get_float = [&config](const char *key, double def) {
        const ConfigOption *opt = config.option(key);
        return opt != nullptr ? opt->getFloat() : def;
    };
    static const std::vector<std::string>   no_colors;
    static const std::vector<unsigned char> no_flags;
    const auto *colors_opt = config.option<ConfigOptionStrings>("filament_colour");
    const auto *mixed_opt  = config.option<ConfigOptionBools>("filament_is_mixed");
    const std::vector<std::string>   &colors   = colors_opt != nullptr ? colors_opt->values : no_colors;
    const std::vector<unsigned char> &is_mixed = mixed_opt != nullptr ? mixed_opt->values : no_flags;
    const int n = int(colors.size());
    auto physical = [&](int idx) { return idx >= 0 && idx < n && (size_t(idx) >= is_mixed.size() || !is_mixed[idx]); };

    int    brightest = -1, darkest = -1;
    double max_l = -1., min_l = 2.;
    for (int i = 0; i < n; ++i) {
        if (!physical(i))
            continue;
        const double l = filament_luminance(colors[i]);
        if (l < 0.)
            continue;
        if (l > max_l) { max_l = l; brightest = i; }
        if (l < min_l) { min_l = l; darkest = i; }
    }

    const int light = get_int("shading_light_filament", 0) - 1;
    const int dark  = get_int("shading_dark_filament", 0) - 1;
    // Automatic choice only when the extreme filament is really light or dark.
    s.light_filament = physical(light) ? light : (max_l >= 0.6 ? brightest : -1);
    s.dark_filament  = physical(dark) ? dark : (min_l >= 0. && min_l <= 0.3 ? darkest : -1);
    const auto *mode = config.option<ConfigOptionEnum<ShadingMode>>("shading_mode");
    s.sublayer       = mode != nullptr && mode->value == ShadingMode::Sublayer;
    s.wall_depth     = std::max(1, get_int("shading_wall_depth", 1));
    s.strength       = std::clamp(get_float("shading_strength", 100.) / 100., 0., 1.);
    s.min_sublayer   = std::max(0.01, get_float("shading_min_sublayer_height", 0.04));
    s.outer_ratio    = std::clamp(get_float("shading_outer_wall_ratio", 0.) / 100., 0., 1.);
    return s;
}

std::vector<unsigned int> used_shading_filaments(const ModelObject &object, const ShadeSettings &settings)
{
    bool darker = false, lighter = false;
    for (const ModelVolume *mv : object.volumes) {
        if (!mv->is_model_part() || !mv->is_shade_painted())
            continue;
        for (int state = 1; state <= 2 * kLevels && !(darker && lighter); ++state) {
            bool &flag = state <= kLevels ? darker : lighter;
            if (!flag && TriangleSelector::has_facets(mv->shade_facets.get_data(), EnforcerBlockerType(state)))
                flag = true;
        }
    }
    std::vector<unsigned int> out;
    if (darker && settings.dark_filament >= 0)
        out.push_back(settings.dark_filament);
    if (lighter && settings.light_filament >= 0)
        out.push_back(settings.light_filament);
    return out;
}

ShadePlan plan_shade(const ShadeSettings &settings, const ShadeTag &tag, size_t layer_id, double print_z, double layer_height, int main_filament)
{
    // The first layer sticks to the bed, it is never shaded.
    if (tag.empty() || layer_id == 0 || layer_height <= 0.)
        return {};
    const int filament = settings.filament_for(tag.level);
    if (filament < 0 || filament == main_filament)
        return {};

    double density = std::abs(int(tag.level)) / double(kLevels) * settings.strength;
    if (tag.outer) {
        if (!settings.sublayer)
            return {};
        density *= settings.outer_ratio;
    }
    density = std::clamp(density, 0., 1.);
    if (density <= 0.)
        return {};

    if (!settings.sublayer) {
        // Spread the shaded layers evenly over the height (Bresenham along the layers).
        const double before = std::floor(double(layer_id) * density + 0.5);
        const double after  = std::floor(double(layer_id + 1) * density + 0.5);
        return after > before ? ShadePlan{ filament, layer_height } : ShadePlan{};
    }

    // Sublayer thickness: the shaded height follows density * z, quantized to the minimum sublayer height,
    // so the rounding error is carried over to the next layers instead of being lost.
    const double q        = settings.min_sublayer;
    auto         quantize = [q](double x) { return std::floor(x / q + 0.5) * q; };
    double       height   = quantize(density * print_z) - quantize(density * (print_z - layer_height));
    if (height < 0.5 * q)
        return {};
    if (height > layer_height - 0.5 * q)
        height = layer_height;
    return { filament, height };
}

struct ShadeField::Impl
{
    std::vector<Vec3f>           vertices;
    std::vector<stl_triangle_vertex_indices> faces;
    std::vector<int8_t>          levels;
    AABBTreeIndirect::Tree3f     tree;
};

ShadeField::ShadeField() : m_impl(std::make_unique<Impl>()) {}
ShadeField::~ShadeField() = default;

std::unique_ptr<ShadeField> ShadeField::build(const PrintObject &object)
{
    const ModelObject *mo = object.model_object();
    if (mo == nullptr || !mo->is_shade_painted())
        return nullptr;

    std::unique_ptr<ShadeField> field(new ShadeField());
    Impl                       &impl  = *field->m_impl;
    const Transform3d           trafo = object.trafo_centered();

    auto append = [&impl](indexed_triangle_set &&its, const Transform3d &tr, int level) {
        its_transform(its, tr);
        const int offset = int(impl.vertices.size());
        impl.vertices.insert(impl.vertices.end(), its.vertices.begin(), its.vertices.end());
        for (const stl_triangle_vertex_indices &f : its.indices) {
            const Vec3f n = (its.vertices[f(1)] - its.vertices[f(0)]).cross(its.vertices[f(2)] - its.vertices[f(0)]);
            const float len = n.norm();
            if (len <= 0.f || std::abs(n.z()) > kMaxSideNormalZ * len)
                continue;
            impl.faces.emplace_back(f(0) + offset, f(1) + offset, f(2) + offset);
            impl.levels.emplace_back(int8_t(level));
        }
    };

    for (const ModelVolume *mv : mo->volumes) {
        if (!mv->is_model_part())
            continue;
        const Transform3d tr = trafo * mv->get_matrix();
        if (!mv->is_shade_painted()) {
            append(indexed_triangle_set(mv->mesh().its), tr, 0);
            continue;
        }
        std::vector<indexed_triangle_set> per_state;
        mv->shade_facets.get_facets(*mv, per_state);
        for (size_t state = 0; state < per_state.size(); ++state)
            if (!per_state[state].indices.empty())
                append(std::move(per_state[state]), tr, state_to_level(int(state)));
    }

    if (impl.faces.empty())
        return nullptr;
    impl.tree = AABBTreeIndirect::build_aabb_tree_over_indexed_triangle_set(impl.vertices, impl.faces);
    return field;
}

int ShadeField::level_at(const Vec3d &point, double max_dist) const
{
    size_t      hit_idx = 0;
    Vec3f       hit_point;
    const Vec3f p  = point.cast<float>();
    const float d2 = AABBTreeIndirect::squared_distance_to_indexed_triangle_set(m_impl->vertices, m_impl->faces, m_impl->tree, p, hit_idx, hit_point);
    if (d2 < 0.f || double(d2) > max_dist * max_dist || hit_idx >= m_impl->levels.size())
        return 0;
    return m_impl->levels[hit_idx];
}

namespace {

using ShadeGroups = std::map<std::pair<int8_t, bool>, ExtrusionEntityCollection>;

struct WallSplitter
{
    const ShadeField    &field;
    double               z;
    double               inner_max_dist;
    double               outer_max_dist; // 0 = do not split the outer wall
    ShadeGroups         &groups;

    // Splits a path into runs of one shade level. Returns false when nothing in it is shaded.
    bool split_path(const ExtrusionPath &path, ExtrusionPaths &main_out)
    {
        const bool outer = path.role() == erExternalPerimeter;
        if (path.role() != erPerimeter && !(outer && outer_max_dist > 0.))
            return false;
        const Points &pts = path.polyline.points;
        if (pts.size() < 2)
            return false;
        const double max_dist = outer ? outer_max_dist : inner_max_dist;

        // Sample the level along the path in short pieces.
        struct Piece { int level; Point end; bool vertex; double length; };
        std::vector<Piece> pieces;
        const double step = scale_(kSampleStep);
        bool any = false;
        for (size_t i = 0; i + 1 < pts.size(); ++i) {
            const Vec2d a = pts[i].cast<double>(), b = pts[i + 1].cast<double>();
            const double len = (b - a).norm();
            const int    n   = std::max(1, int(std::ceil(len / step)));
            for (int k = 0; k < n; ++k) {
                const Vec2d mid   = a + (b - a) * ((k + 0.5) / n);
                const int   level = field.level_at(Vec3d(unscale_(mid.x()), unscale_(mid.y()), z), max_dist);
                any |= level != 0;
                const bool  vertex = k + 1 == n;
                pieces.push_back({ level, vertex ? pts[i + 1] : Point((a + (b - a) * (double(k + 1) / n)).cast<coord_t>()), vertex, len / n });
            }
        }
        if (!any)
            return false;

        // Group the pieces into runs of one level.
        struct Run { int level; size_t begin; size_t end; double length; };
        std::vector<Run> runs;
        for (size_t i = 0; i < pieces.size(); ++i) {
            if (runs.empty() || runs.back().level != pieces[i].level)
                runs.push_back({ pieces[i].level, i, i, 0. });
            runs.back().end = i + 1;
            runs.back().length += pieces[i].length;
        }
        // Too short runs flicker, merge them into the previous (or next) run.
        const double min_len = scale_(kMinRunLength);
        for (size_t i = 0; i < runs.size() && runs.size() > 1; ++i)
            if (runs[i].length < min_len)
                runs[i].level = i > 0 ? runs[i - 1].level : runs[i + 1].level;
        std::vector<Run> merged;
        for (const Run &run : runs) {
            if (!merged.empty() && merged.back().level == run.level) {
                merged.back().end = run.end;
                merged.back().length += run.length;
            } else
                merged.push_back(run);
        }
        if (std::all_of(merged.begin(), merged.end(), [](const Run &r) { return r.level == 0; }))
            return false;

        for (const Run &run : merged) {
            // Keep only the original vertices and the points where the level changes.
            Points points{ run.begin == 0 ? pts.front() : pieces[run.begin - 1].end };
            for (size_t i = run.begin; i < run.end; ++i)
                if (pieces[i].vertex || i + 1 == run.end)
                    points.push_back(pieces[i].end);
            if (points.size() < 2)
                continue;
            ExtrusionPath piece(path);
            piece.polyline.points = std::move(points);
            if (run.level == 0)
                main_out.emplace_back(std::move(piece));
            else
                groups[{ int8_t(run.level), outer }].append(std::move(piece));
        }
        return true;
    }

    // Splits the paths of a loop or multi path. Returns false when nothing in it is shaded.
    bool split_paths(const ExtrusionPaths &paths, ExtrusionPaths &main_out)
    {
        bool any = false;
        ExtrusionPaths out;
        for (const ExtrusionPath &path : paths) {
            if (split_path(path, out))
                any = true;
            else
                out.push_back(path);
        }
        if (any)
            main_out = std::move(out);
        return any;
    }

    void process(ExtrusionEntityCollection &collection)
    {
        ExtrusionEntitiesPtr entities;
        entities.reserve(collection.entities.size());
        for (ExtrusionEntity *entity : collection.entities) {
            ExtrusionPaths main;
            bool           split = false;
            if (auto *sub = dynamic_cast<ExtrusionEntityCollection *>(entity)) {
                this->process(*sub);
            } else if (auto *loop = dynamic_cast<ExtrusionLoop *>(entity)) {
                split = this->split_paths(loop->paths, main);
            } else if (auto *multi = dynamic_cast<ExtrusionMultiPath *>(entity)) {
                split = this->split_paths(multi->paths, main);
            } else if (auto *path = dynamic_cast<ExtrusionPath *>(entity)) {
                split = this->split_path(*path, main);
            }
            if (!split) {
                entities.push_back(entity);
                continue;
            }
            delete entity;
            for (ExtrusionPath &piece : main)
                entities.push_back(piece.clone_move());
        }
        collection.entities = std::move(entities);
    }
};

} // namespace

void split_shaded_walls(Layer &layer, const ShadeField &field, const ShadeSettings &settings)
{
    for (LayerRegion *layerm : layer.regions()) {
        layerm->perimeters_shade.clear();
        if (layerm->perimeters.entities.empty())
            continue;

        const Flow ext = layerm->flow(frExternalPerimeter);
        const Flow per = layerm->flow(frPerimeter);
        // Distance from the surface to the center of the deepest shaded inner wall plus a margin of almost half a line.
        const double first_inner    = ext.width() + 0.5 * per.spacing();
        const double inner_max_dist = first_inner + (settings.wall_depth - 1) * per.spacing() + 0.45 * per.spacing();
        const double outer_max_dist = settings.sublayer && settings.outer_ratio > 0. ? ext.width() : 0.;

        ExtrusionEntitiesPtr           entities;
        std::vector<Shading::ShadeTag> tags;
        bool                           any = false;
        for (ExtrusionEntity *entity : layerm->perimeters.entities) {
            auto *island = dynamic_cast<ExtrusionEntityCollection *>(entity);
            if (island != nullptr) {
                ShadeGroups  groups;
                WallSplitter splitter{ field, layer.slice_z, inner_max_dist, outer_max_dist, groups };
                splitter.process(*island);
                // Shade pieces go in front of their island, so that a lower sublayer is printed before the outer wall.
                for (auto &[key, collection] : groups) {
                    if (collection.entities.empty())
                        continue;
                    entities.push_back(new ExtrusionEntityCollection(std::move(collection)));
                    tags.push_back({ key.first, key.second });
                    any = true;
                }
                if (island->entities.empty()) {
                    delete island;
                    continue;
                }
            }
            entities.push_back(entity);
            tags.push_back({});
        }
        layerm->perimeters.entities = std::move(entities);
        if (any)
            layerm->perimeters_shade = std::move(tags);
    }
}

} // namespace Shading
} // namespace Slic3r

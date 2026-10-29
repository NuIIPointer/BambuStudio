#include "Shading.hpp"

#include <cmath>
#include <map>

#include <boost/log/trivial.hpp>

#include "AABBTreeIndirect.hpp"
#include "ClipperUtils.hpp"
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
// Sampling step along the walls, the window the levels are averaged over and the shortest run of one level that is kept.
constexpr double kSampleStep    = 0.3;
constexpr double kSmoothWindow  = 2.0;
constexpr double kMinRunLength  = 2.0;
// Automatic choice of the shading filaments: only really light or dark filaments qualify (a saturated red is about 0.3).
constexpr double kMinAutoLight = 0.7;
constexpr double kMaxAutoDark  = 0.2;
// Shaded skins stay this far inside the fill area: the nozzle printing a lower sublayer would touch the walls.
constexpr double kSkinWallClearance = 0.6;
// Shorter shaded skin pieces stay in the main color.
constexpr double kMinSkinPiece = 0.6;

} // namespace

ShadeSettings ShadeSettings::from_config(const ConfigBase &config, const ConfigBase &object_config)
{
    ShadeSettings s;
    auto get_int = [](const ConfigBase &cfg, const char *key, int def) {
        const ConfigOption *opt = cfg.option(key);
        return opt != nullptr ? opt->getInt() : def;
    };
    auto get_float = [&object_config](const char *key, double def) {
        const ConfigOption *opt = object_config.option(key);
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

    const int light = get_int(config, "shading_light_filament", 0) - 1;
    const int dark  = get_int(config, "shading_dark_filament", 0) - 1;
    // Automatic choice only when the extreme filament is really light or dark.
    s.light_filament = physical(light) ? light : (max_l >= kMinAutoLight ? brightest : -1);
    s.dark_filament  = physical(dark) ? dark : (min_l >= 0. && min_l <= kMaxAutoDark ? darkest : -1);
    const auto *mode = object_config.option<ConfigOptionEnum<ShadingMode>>("shading_mode");
    s.sublayer       = mode != nullptr && mode->value == ShadingMode::Sublayer;
    s.wall_depth     = std::max(1, get_int(object_config, "shading_wall_depth", 1));
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

namespace {

// A shading filament never prints a sublayer piece at the full layer height: the main filament keeps the top part.
// Otherwise the second shading filament could print a lower sublayer next to it later in the layer.
double max_sublayer_height(const ShadeSettings &settings, double layer_height)
{
    return std::max(0.5 * layer_height, layer_height - settings.min_sublayer);
}

} // namespace

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
    return { filament, std::min(height, max_sublayer_height(settings, layer_height)) };
}

ShadePlan plan_skin_shade(const ShadeSettings &settings, const ShadeTag &tag, size_t layer_id, double layer_height, int main_filament)
{
    if (tag.empty() || layer_id == 0 || layer_height <= 0.)
        return {};
    const int filament = settings.filament_for(tag.level);
    if (filament < 0 || filament == main_filament)
        return {};
    const double density = std::clamp(std::abs(int(tag.level)) / double(kLevels) * settings.strength, 0., 1.);
    const double cover   = tag.cover > 0.f ? std::min(double(tag.cover), layer_height) : layer_height;
    const double q       = settings.min_sublayer;
    // Never thicker than the part of the layer inside the zone, so a thin rest of the zone stays in the main color.
    const double height  = std::min(std::floor(density * cover / q + 0.5), std::floor(cover / q + EPSILON)) * q;
    if (height < 0.5 * q)
        return {};
    return { filament, std::min(height, max_sublayer_height(settings, layer_height)) };
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

        // Painted levels often change from one small triangle to the next (and a spray is grainy on purpose).
        // Average them along the wall, so that a wall piece does not change its filament every few tenths of a mm.
        {
            const int half_window = std::max(1, int(std::lround(0.5 * kSmoothWindow / kSampleStep)));
            std::vector<int> smoothed(pieces.size());
            for (int i = 0; i < int(pieces.size()); ++i) {
                double sum = 0.;
                int    cnt = 0;
                for (int k = std::max(0, i - half_window); k <= std::min(int(pieces.size()) - 1, i + half_window); ++k, ++cnt)
                    sum += pieces[k].level;
                smoothed[i] = int(std::lround(sum / cnt));
            }
            for (size_t i = 0; i < pieces.size(); ++i)
                pieces[i].level = smoothed[i];
        }

        // Group the pieces into runs of one level.
        struct Run { int level; size_t begin; size_t end; double length; };
        std::vector<Run> merged;
        for (size_t i = 0; i < pieces.size(); ++i) {
            if (merged.empty() || merged.back().level != pieces[i].level)
                merged.push_back({ pieces[i].level, i, i, 0. });
            merged.back().end = i + 1;
            merged.back().length += pieces[i].length;
        }
        // Too short runs flicker. Merge the shortest one into the neighbor with the closer level until none is left;
        // this does not cascade, every merge looks at the current neighbors.
        const double min_len = scale_(kMinRunLength);
        while (merged.size() > 1) {
            size_t shortest = 0;
            for (size_t i = 1; i < merged.size(); ++i)
                if (merged[i].length < merged[shortest].length)
                    shortest = i;
            if (merged[shortest].length >= min_len)
                break;
            size_t into;
            if (shortest == 0)
                into = 1;
            else if (shortest + 1 == merged.size())
                into = shortest - 1;
            else
                into = std::abs(merged[shortest - 1].level - merged[shortest].level) <= std::abs(merged[shortest + 1].level - merged[shortest].level) ?
                           shortest - 1 : shortest + 1;
            Run &target = merged[into];
            target.begin = std::min(target.begin, merged[shortest].begin);
            target.end   = std::max(target.end, merged[shortest].end);
            target.length += merged[shortest].length;
            merged.erase(merged.begin() + shortest);
            // Neighbors of the same level become one run.
            for (size_t i = 1; i < merged.size();) {
                if (merged[i - 1].level == merged[i].level) {
                    merged[i - 1].end = merged[i].end;
                    merged[i - 1].length += merged[i].length;
                    merged.erase(merged.begin() + i);
                } else
                    ++i;
            }
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
                    BOOST_LOG_TRIVIAL(trace) << "Shading: layer " << layer.id() << " level " << int(key.first) << (key.second ? " outer" : " inner")
                                             << " pieces " << collection.entities.size();
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

struct SkinShadeField::Impl
{
    struct Facet
    {
        Polygon xy;
        float   z_min;
        float   z_max;
        int8_t  level;
        bool    top;
    };
    std::vector<Facet> facets; // sorted by z_min
};

SkinShadeField::SkinShadeField() : m_impl(new Impl()) {}
SkinShadeField::~SkinShadeField() = default;

std::unique_ptr<SkinShadeField> SkinShadeField::build(const PrintObject &object)
{
    const ModelObject *mo = object.model_object();
    if (mo == nullptr || !mo->is_shade_painted())
        return nullptr;

    std::unique_ptr<SkinShadeField> field(new SkinShadeField());
    std::vector<Impl::Facet>       &facets = field->m_impl->facets;
    const Transform3d               trafo  = object.trafo_centered();
    for (const ModelVolume *mv : mo->volumes) {
        if (!mv->is_model_part() || !mv->is_shade_painted())
            continue;
        const Transform3d                 tr = trafo * mv->get_matrix();
        std::vector<indexed_triangle_set> per_state;
        mv->shade_facets.get_facets(*mv, per_state);
        for (size_t state = 1; state < per_state.size(); ++state) {
            const int level = state_to_level(int(state));
            if (level == 0 || per_state[state].indices.empty())
                continue;
            indexed_triangle_set &its = per_state[state];
            its_transform(its, tr);
            for (const stl_triangle_vertex_indices &f : its.indices) {
                const Vec3f &a = its.vertices[f(0)], &b = its.vertices[f(1)], &c = its.vertices[f(2)];
                const Vec3f  n   = (b - a).cross(c - a);
                const float  len = n.norm();
                if (len <= 0.f || std::abs(n.z()) <= kMaxSideNormalZ * len)
                    continue;
                Impl::Facet facet;
                facet.xy.points = { Point(scale_(a.x()), scale_(a.y())), Point(scale_(b.x()), scale_(b.y())), Point(scale_(c.x()), scale_(c.y())) };
                if (facet.xy.area() < 0.)
                    facet.xy.reverse();
                if (facet.xy.area() <= 0.)
                    continue;
                facet.z_min = std::min({ a.z(), b.z(), c.z() });
                facet.z_max = std::max({ a.z(), b.z(), c.z() });
                facet.level = int8_t(level);
                facet.top   = n.z() > 0.f;
                facets.emplace_back(std::move(facet));
            }
        }
    }
    if (facets.empty())
        return nullptr;
    std::sort(facets.begin(), facets.end(), [](const Impl::Facet &l, const Impl::Facet &r) { return l.z_min < r.z_min; });
    return field;
}

std::vector<std::pair<int, ExPolygons>> SkinShadeField::areas(bool top, double z_min, double z_max) const
{
    std::map<int, Polygons> by_level;
    for (const Impl::Facet &facet : m_impl->facets) {
        if (facet.z_min > z_max)
            break;
        if (facet.top == top && facet.z_max >= z_min)
            by_level[facet.level].push_back(facet.xy);
    }
    std::vector<std::pair<int, ExPolygons>> out;
    for (auto &[level, polygons] : by_level) {
        ExPolygons area = union_ex(polygons);
        if (!area.empty())
            out.emplace_back(level, std::move(area));
    }
    return out;
}

namespace {

// Shade area of a skin layer: the painted level and the thickness of the layer inside the shade zone (scaled).
using SkinKey   = std::pair<int, coord_t>;
using SkinAreas = std::vector<std::pair<SkinKey, ExPolygons>>;

struct SkinSplitter
{
    const SkinAreas                              &areas;
    const ExPolygons                             &all; // union of the areas
    std::map<SkinKey, ExtrusionEntityCollection> &groups;

    static ExtrusionPath *make_path(const ExtrusionPath &model, Polyline &&polyline)
    {
        auto *path     = new ExtrusionPath(model);
        path->polyline = std::move(polyline);
        return path;
    }

    // Cuts the solid infill of entity along the shade areas into groups, the unshaded rest goes to main_out.
    void split(const ExtrusionEntity &entity, ExtrusionEntitiesPtr &main_out)
    {
        if (const auto *collection = dynamic_cast<const ExtrusionEntityCollection *>(&entity)) {
            auto *rest    = new ExtrusionEntityCollection();
            rest->no_sort = collection->no_sort;
            for (const ExtrusionEntity *ee : collection->entities)
                this->split(*ee, rest->entities);
            if (rest->entities.empty())
                delete rest;
            else
                main_out.push_back(rest);
            return;
        }
        ExtrusionPaths paths;
        if (const auto *path = dynamic_cast<const ExtrusionPath *>(&entity))
            paths.push_back(*path);
        else if (const auto *multi = dynamic_cast<const ExtrusionMultiPath *>(&entity))
            paths = multi->paths;
        else if (const auto *loop = dynamic_cast<const ExtrusionLoop *>(&entity))
            paths = loop->paths;
        // Keep the entity as it is unless one of its solid infill paths reaches into a shade area.
        auto reaches = [this](const ExtrusionPath &p) { return p.role() == erSolidInfill && !intersection_pl(p.polyline, all).empty(); };
        if (std::none_of(paths.begin(), paths.end(), reaches)) {
            main_out.push_back(entity.clone());
            return;
        }
        for (const ExtrusionPath &path : paths) {
            if (path.role() != erSolidInfill) {
                main_out.push_back(new ExtrusionPath(path));
                continue;
            }
            Polylines rest = diff_pl(path.polyline, all);
            for (const auto &[key, area] : areas)
                for (Polyline &piece : intersection_pl(path.polyline, area)) {
                    if (piece.length() < scale_(kMinSkinPiece))
                        rest.push_back(std::move(piece));
                    else
                        groups[key].entities.push_back(make_path(path, std::move(piece)));
                }
            for (Polyline &piece : rest)
                main_out.push_back(make_path(path, std::move(piece)));
        }
    }
};

} // namespace

void split_shaded_skins(Layer &layer, const SkinShadeField &field)
{
    for (LayerRegion *layerm : layer.regions())
        layerm->fills_shade.clear();

    for (LayerRegion *layerm : layer.regions()) {
        if (layerm->fills.entities.empty())
            continue;
        // The top and bottom layers keep the main color. The shade zone behind them is one inner wall width thick,
        // like the shaded inner wall behind the outer wall. Closer surfaces win where zones overlap.
        const double zone  = layerm->flow(frPerimeter).width();
        const double z_top = layer.print_z, z_bottom = layer.print_z - layer.height;
        const ExPolygons inner = offset_ex(layerm->fill_expolygons, -float(scale_(kSkinWallClearance)));
        SkinAreas        region_areas;
        ExPolygons       taken;
        auto add_surface = [&](bool top, const Layer *surface_layer, const Layer *beyond) {
            // Part of this layer inside the zone of the surface layer.
            const double zone_min = top ? surface_layer->print_z - surface_layer->height - zone : surface_layer->print_z;
            const double zone_max = top ? surface_layer->print_z - surface_layer->height : surface_layer->print_z + zone;
            const double cover    = std::min(z_top, zone_max) - std::max(z_bottom, zone_min);
            if (cover <= EPSILON)
                return;
            const ExPolygons surface = beyond != nullptr ? diff_ex(surface_layer->lslices, beyond->lslices) : surface_layer->lslices;
            if (surface.empty())
                return;
            // Generous z range: a painted surface of another layer only matches where that layer is covered.
            const double z = top ? surface_layer->slice_z + 0.5 * surface_layer->height : surface_layer->slice_z - 0.5 * surface_layer->height;
            for (auto &[level, area] : field.areas(top, z - surface_layer->height, z + surface_layer->height)) {
                ExPolygons shaded = diff_ex(intersection_ex(intersection_ex(area, surface), inner), taken);
                if (shaded.empty())
                    continue;
                append(taken, shaded);
                region_areas.emplace_back(SkinKey{ level, scale_(std::min(cover, layer.height)) }, std::move(shaded));
            }
            taken = union_ex(taken);
        };
        for (const Layer *upper = layer.upper_layer; upper != nullptr && upper->print_z - upper->height - zone < z_top - EPSILON; upper = upper->upper_layer)
            add_surface(true, upper, upper->upper_layer);
        for (const Layer *lower = layer.lower_layer; lower != nullptr && lower->print_z + zone > z_bottom + EPSILON; lower = lower->lower_layer)
            add_surface(false, lower, lower->lower_layer);
        if (region_areas.empty())
            continue;
        const ExPolygons all = union_ex(taken);

        ExtrusionEntitiesPtr           entities;
        std::vector<Shading::ShadeTag> tags;
        bool                           any = false;
        for (ExtrusionEntity *entity : layerm->fills.entities) {
            std::map<SkinKey, ExtrusionEntityCollection> groups;
            ExtrusionEntitiesPtr                         rest;
            SkinSplitter{ region_areas, all, groups }.split(*entity, rest);
            if (groups.empty()) {
                for (ExtrusionEntity *ee : rest)
                    delete ee;
                entities.push_back(entity);
                tags.push_back({});
                continue;
            }
            for (auto &[key, collection] : groups) {
                BOOST_LOG_TRIVIAL(trace) << "Shading: layer " << layer.id() << " skin level " << key.first << " cover " << unscale<double>(key.second) << " pieces " << collection.entities.size();
                entities.push_back(new ExtrusionEntityCollection(std::move(collection)));
                tags.push_back({ int8_t(key.first), false, true, float(unscale<double>(key.second)) });
            }
            for (ExtrusionEntity *ee : rest) {
                entities.push_back(ee);
                tags.push_back({});
            }
            delete entity;
            any = true;
        }
        layerm->fills.entities = std::move(entities);
        if (any)
            layerm->fills_shade = std::move(tags);
    }
}

} // namespace Shading
} // namespace Slic3r

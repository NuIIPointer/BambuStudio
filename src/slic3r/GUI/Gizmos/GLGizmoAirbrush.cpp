#include "GLGizmoAirbrush.hpp"

#include "libslic3r/Model.hpp"
#include "libslic3r/PresetBundle.hpp"
#include "libslic3r/Shading.hpp"

#include "slic3r/GUI/Camera.hpp"
#include "slic3r/GUI/GLCanvas3D.hpp"
#include "slic3r/GUI/GUI_App.hpp"
#include "slic3r/GUI/GUI_ObjectList.hpp"
#include "slic3r/GUI/ImGuiWrapper.hpp"
#include "slic3r/GUI/MeshUtils.hpp"
#include "slic3r/GUI/Plater.hpp"
#include "slic3r/GUI/format.hpp"
#include "slic3r/Utils/UndoRedo.hpp"

#include <GL/glew.h>

namespace Slic3r::GUI {

namespace {

constexpr float StrengthMin = 0.25f;
constexpr float StrengthMax = 8.f;

// Shade in [-1, 1] -> painted state.
EnforcerBlockerType shade_to_state(float shade)
{
    const int level = int(std::lround(std::clamp(shade, -1.f, 1.f) * Shading::kLevels));
    return EnforcerBlockerType(Shading::level_to_state(level));
}

} // namespace

GLGizmoAirbrush::GLGizmoAirbrush(GLCanvas3D &parent, unsigned int sprite_id)
    : GLGizmoPainterBase(parent, sprite_id)
{
    m_current_tool = ImGui::CircleButtonIcon;
    m_tool_type    = ToolType::BRUSH;
    m_cursor_type  = TriangleSelector::CursorType::CIRCLE;
    m_cursor_radius = 3.f;
}

void GLGizmoAirbrush::data_changed(bool is_serializing)
{
    set_painter_gizmo_data(m_parent.get_selection());
}

void GLGizmoAirbrush::on_shutdown()
{
    m_gradient = {};
    m_parent.use_slope(false);
    m_parent.toggle_model_objects_visibility(true);
}

std::string GLGizmoAirbrush::on_get_name() const
{
    if (!on_is_activable() && m_state == EState::Off)
        return _u8L("Airbrush shading") + ":\n" + _u8L("Please select single object.");
    return _u8L("Airbrush shading");
}

bool GLGizmoAirbrush::on_init()
{
    m_shortcut_key = WXK_CONTROL_J;
    const wxString ctrl  = GUI::shortkey_ctrl_prefix();
    const wxString alt   = GUI::shortkey_alt_prefix();
    const wxString shift = "Shift+";
    m_desc["clipping_of_view_caption"] = alt + _L("Mouse wheel");
    m_desc["clipping_of_view"]         = _L("Section view");
    m_desc["reset_direction"]          = _L("Reset direction");
    m_desc["cursor_size_caption"]      = ctrl + _L("Mouse wheel");
    m_desc["cursor_size"]              = _L("Pen size");
    m_desc["spray_caption"]            = _L("Left mouse button");
    m_desc["spray"]                    = _L("Spray the chosen direction");
    m_desc["spray_opposite_caption"]   = _L("Right mouse button");
    m_desc["spray_opposite"]           = _L("Spray the opposite direction");
    m_desc["erase_caption"]            = shift + _L("Left mouse button");
    m_desc["erase"]                    = _L("Erase shading");
    m_desc["fill_caption"]             = _L("Left mouse button");
    m_desc["fill"]                     = _L("Fill the area with the shade");
    m_desc["fill_erase_caption"]       = _L("Right mouse button");
    m_desc["fill_erase"]               = _L("Erase the area");
    m_desc["gradient_caption"]         = _L("Left mouse drag");
    m_desc["gradient"]                 = _L("Gradient across the area under the start point");
    m_desc["smart_fill_angle_caption"] = ctrl + _L("Mouse wheel");
    m_desc["smart_fill_angle"]         = _L("Smart fill angle");
    m_desc["remove_all"]               = _L("Erase all shading");
    m_desc["tool_type"]                = _L("Tool type");
    m_desc["darker"]                   = _L("Darker");
    m_desc["lighter"]                  = _L("Lighter");
    m_desc["strength"]                 = _L("Strength");
    m_desc["hardness"]                 = _L("Hardness");
    m_desc["shade"]                    = _L("Shade");
    m_desc["gradient_from"]            = _L("Start shade");
    m_desc["gradient_to"]              = _L("End shade");
    return true;
}

std::string GLGizmoAirbrush::get_icon_filename(bool is_dark_mode) const
{
    return is_dark_mode ? "toolbar_airbrush_dark.svg" : "toolbar_airbrush.svg";
}

EnforcerBlockerType GLGizmoAirbrush::get_left_button_state_type() const
{
    return shade_to_state(m_fill_shade);
}

bool GLGizmoAirbrush::on_key_down_select_tool_type(int keyCode)
{
    switch (keyCode) {
    case 'C': m_current_tool = ImGui::CircleButtonIcon; break;
    case 'S': m_current_tool = ImGui::SphereButtonIcon; break;
    case 'F': m_current_tool = ImGui::FillButtonIcon; break;
    case 'G': m_current_tool = ImGui::HeightRangeIcon; break;
    default: return false;
    }
    return true;
}

void GLGizmoAirbrush::render_painter_gizmo() const
{
    const Selection &selection = m_parent.get_selection();

    glsafe(::glEnable(GL_BLEND));
    glsafe(::glEnable(GL_DEPTH_TEST));

    render_triangles(selection);

    m_c->object_clipper()->render_cut();
    m_c->instances_hider()->render_cut();
    if (m_current_tool != ImGui::HeightRangeIcon)
        render_cursor();

    glsafe(::glDisable(GL_BLEND));
}

void GLGizmoAirbrush::render_triangles(const Selection &selection) const
{
    ClippingPlaneDataWrapper clp_data = this->get_clipping_plane_data();
    const auto              &shader   = wxGetApp().get_shader("mm_gouraud");
    if (!shader)
        return;
    wxGetApp().bind_shader(shader);
    shader->set_uniform("clipping_plane", clp_data.clp_dataf);
    shader->set_uniform("z_range", clp_data.z_range);
    shader->set_uniform("slope.actived", m_parent.is_using_slope());
    ScopeGuard guard([shader]() {
        if (shader)
            wxGetApp().unbind_shader();
    });

    glsafe(::glDisable(GL_CULL_FACE));

    const ModelObject *mo      = m_c->selection_info()->model_object();
    int                mesh_id = -1;
    for (const ModelVolume *mv : mo->volumes) {
        if (!mv->is_model_part())
            continue;
        ++mesh_id;
        if (mesh_id >= int(m_triangle_selectors.size()))
            break;

        Transform3d trafo_matrix;
        if (m_parent.get_canvas_type() == GLCanvas3D::CanvasAssembleView) {
            trafo_matrix = mo->instances[selection.get_instance_idx()]->get_assemble_transformation().get_matrix() * mv->get_assemble_transformation().get_matrix();
            trafo_matrix.translate(mv->get_transformation().get_offset() * (GLVolume::explosion_ratio - 1.0) +
                                   mo->instances[selection.get_instance_idx()]->get_offset_to_assembly() * (GLVolume::explosion_ratio - 1.0));
        } else {
            trafo_matrix = mo->instances[selection.get_instance_idx()]->get_transformation().get_matrix() * mv->get_matrix();
        }

        const bool is_left_handed = trafo_matrix.matrix().determinant() < 0.;
        if (is_left_handed)
            glsafe(::glFrontFace(GL_CW));

        const Camera     &camera = wxGetApp().plater()->get_camera();
        const Transform3d matrix = camera.get_view_matrix() * trafo_matrix;
        shader->set_uniform("view_model_matrix", matrix);
        shader->set_uniform("projection_matrix", camera.get_projection_matrix());
        shader->set_uniform("normal_matrix", (Matrix3d) matrix.matrix().block(0, 0, 3, 3).inverse().transpose());
        shader->set_uniform("volume_world_matrix", trafo_matrix);
        shader->set_uniform("volume_mirrored", is_left_handed);
        m_triangle_selectors[mesh_id]->render(m_imgui, trafo_matrix);

        if (is_left_handed)
            glsafe(::glFrontFace(GL_CCW));
    }
}

void GLGizmoAirbrush::on_set_state()
{
    GLGizmoPainterBase::on_set_state();
    if (get_state() == Off) {
        if (ModelObject *mo = m_c->selection_info()->model_object())
            Slic3r::save_object_mesh(*mo);
    }
}

bool GLGizmoAirbrush::paint_brush_stroke(TriangleSelectorGUI                       &selector,
                                         const std::vector<ProjectedMousePosition> &positions,
                                         const Vec3f                               &camera_pos,
                                         const Transform3d                         &trafo_matrix,
                                         const TriangleSelector::ClippingPlane     &clp,
                                         Button                                     button,
                                         bool                                       shift_down)
{
    // 0 = erase, -1 = darker, +1 = lighter. The right button sprays the opposite direction.
    const int direction = shift_down ? 0 : (((button == Button::Left) == m_darker) ? -1 : 1);
    std::uniform_real_distribution<float> dither(0.f, 1.f);
    const auto modify = [this, direction, &dither](EnforcerBlockerType old_state, float weight) {
        int level = Shading::state_to_level(int(old_state));
        // Random rounding gives the grainy look of a spray and lets weak sprays build up.
        const int amount = int(std::floor(m_strength * weight + dither(m_rng)));
        if (direction == 0)
            level = level > 0 ? std::max(0, level - 2 * amount) : std::min(0, level + 2 * amount);
        else
            level = std::clamp(level + direction * amount, -Shading::kLevels, Shading::kLevels);
        return EnforcerBlockerType(Shading::level_to_state(level));
    };

    // One spray per third of the brush radius along the stroke.
    const float spacing = m_cursor_radius / 3.f;
    const Vec3f *last   = nullptr;
    for (const ProjectedMousePosition &position : positions) {
        if (last != nullptr && (position.mesh_hit - *last).norm() < spacing)
            continue;
        last = &position.mesh_hit;
        std::unique_ptr<TriangleSelector::Cursor> cursor = TriangleSelector::SinglePointCursor::cursor_factory(position.mesh_hit, camera_pos, m_cursor_radius,
                                                                                                               m_cursor_type, trafo_matrix, clp);
        selector.airbrush_patch(int(position.facet_idx), std::move(cursor), modify, m_hardness);
    }
    return true;
}

int GLGizmoAirbrush::raycast(const Vec2d &mouse_position, Vec3f &hit, size_t &facet, Transform3d &trafo) const
{
    const Camera      &camera    = wxGetApp().plater()->get_camera();
    const Selection   &selection = m_parent.get_selection();
    const ModelObject *mo        = m_c->selection_info()->model_object();
    if (mo == nullptr)
        return -1;

    int    best_mesh = -1;
    double best_dist = std::numeric_limits<double>::max();
    int    mesh_id   = -1;
    for (int volume_id = 0; volume_id < int(mo->volumes.size()); ++volume_id) {
        const ModelVolume *mv = mo->volumes[volume_id];
        if (!mv->is_model_part())
            continue;
        ++mesh_id;
        if (mesh_id >= int(m_c->raycaster()->raycasters().size()))
            break;
        const Transform3d mesh_trafo = get_volume_world_matrix(selection, mo, mv, volume_id);
        Vec3f             mesh_hit, normal;
        size_t            mesh_facet = 0;
        if (!m_c->raycaster()->raycasters()[mesh_id]->unproject_on_mesh(mouse_position, mesh_trafo, camera, mesh_hit, normal,
                                                                          m_c->object_clipper()->get_clipping_plane(), &mesh_facet,
                                                                          m_parent.get_canvas_type() != GLCanvas3D::CanvasAssembleView))
            continue;
        const double dist = (camera.get_position() - mesh_trafo * mesh_hit.cast<double>()).squaredNorm();
        if (dist < best_dist) {
            best_dist = dist;
            best_mesh = mesh_id;
            hit       = mesh_hit;
            facet     = mesh_facet;
            trafo     = mesh_trafo;
        }
    }
    return best_mesh;
}

bool GLGizmoAirbrush::gizmo_event(SLAGizmoEventType action, const Vec2d &mouse_position, bool shift_down, bool alt_down, bool control_down)
{
    if (m_current_tool != ImGui::HeightRangeIcon || m_triangle_selectors.empty())
        return GLGizmoPainterBase::gizmo_event(action, mouse_position, shift_down, alt_down, control_down);

    // Gradient tool: drag a line, the smart fill area under its start gets a gradient along it.
    if (action == SLAGizmoEventType::LeftDown) {
        Vec3f       hit;
        size_t      facet = 0;
        Transform3d trafo;
        const int   mesh_idx = this->raycast(mouse_position, hit, facet, trafo);
        if (mesh_idx < 0 || mesh_idx >= int(m_triangle_selectors.size()))
            return false;
        m_gradient              = {};
        m_gradient.active       = true;
        m_gradient.mesh_idx     = mesh_idx;
        m_gradient.facet        = facet;
        m_gradient.start_mesh   = hit;
        m_gradient.end_mesh     = hit;
        m_gradient.trafo        = trafo;
        m_gradient.start_screen = mouse_position;
        m_gradient.end_screen   = mouse_position;
        return true;
    }
    if (action == SLAGizmoEventType::Dragging && m_gradient.active) {
        m_gradient.end_screen = mouse_position;
        Vec3f       hit;
        size_t      facet = 0;
        Transform3d trafo;
        if (this->raycast(mouse_position, hit, facet, trafo) >= 0)
            // Express the end point in the coordinates of the mesh the gradient starts on.
            m_gradient.end_mesh = (m_gradient.trafo.inverse() * (trafo * hit.cast<double>())).cast<float>();
        m_parent.set_as_dirty();
        return true;
    }
    if (action == SLAGizmoEventType::LeftUp && m_gradient.active) {
        Plater::TakeSnapshot snapshot(wxGetApp().plater(), "Airbrush gradient", UndoRedo::SnapshotType::GizmoAction);
        this->apply_gradient();
        m_gradient.active = false;
        update_model_object();
        m_parent.set_as_dirty();
        return true;
    }
    if (action == SLAGizmoEventType::Dragging || action == SLAGizmoEventType::RightDown || action == SLAGizmoEventType::RightUp)
        return m_gradient.active;
    return GLGizmoPainterBase::gizmo_event(action, mouse_position, shift_down, alt_down, control_down);
}

void GLGizmoAirbrush::apply_gradient()
{
    TriangleSelectorGUI &selector = *m_triangle_selectors[m_gradient.mesh_idx];
    Transform3d trafo_no_translate = m_gradient.trafo;
    trafo_no_translate.translation() = Vec3d::Zero();
    const TriangleSelector::ClippingPlane clp = this->get_clipping_plane_in_volume_coordinates(m_gradient.trafo);

    selector.seed_fill_unselect_all_triangles();
    selector.seed_fill_select_triangles(m_gradient.start_mesh, int(m_gradient.facet), trafo_no_translate, clp, m_smart_fill_angle, 0.f, true);

    const Vec3f start  = m_gradient.start_mesh;
    const Vec3f dir    = m_gradient.end_mesh - m_gradient.start_mesh;
    const float len2   = dir.squaredNorm();
    const float from   = m_gradient_from;
    const float to     = m_gradient_to;
    selector.seed_fill_apply_field(
        [start, dir, len2, from, to](const Vec3f &centroid) {
            const float t = len2 > 1e-6f ? std::clamp((centroid - start).dot(dir) / len2, 0.f, 1.f) : 0.f;
            return shade_to_state(from + (to - from) * t);
        },
        std::clamp(std::sqrt(len2) / 32.f, 0.2f, 1.f));
    selector.request_update_render_data(true);
}

void GLGizmoAirbrush::render_shading_filaments_info()
{
    const Shading::ShadeSettings settings = Shading::ShadeSettings::from_config(wxGetApp().preset_bundle->full_config());
    auto line = [this](const wxString &label, int filament) {
        if (filament >= 0)
            m_imgui->text(label + ": " + wxString::Format(_L("Filament %d"), filament + 1));
        else
            m_imgui->warning_text(label + ": " + _L("no matching filament, set it in the process settings"));
    };
    line(_L("Darker shades"), settings.dark_filament);
    line(_L("Lighter shades"), settings.light_filament);
}

void GLGizmoAirbrush::on_render_input_window(float x, float y, float bottom_limit)
{
    if (!m_c || !m_c->selection_info() || !m_c->selection_info()->model_object())
        return;

    m_imgui_start_pos[0] = x;
    m_imgui_start_pos[1] = y;

    const float approx_height = m_imgui->scaled(26.f);
    y                         = std::min(y, bottom_limit - approx_height);
    GizmoImguiSetNextWIndowPos(x, y, ImGuiCond_Always, 0.0f, 0.0f);

    ImGuiWrapper::push_toolbar_style(m_parent.get_scale());
    GizmoImguiBegin(get_name(), ImGuiWindowFlags_NoMove | ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoTitleBar);

    const float space_size = m_imgui->get_style_scaling() * 8;
    float       sliders_left_width = 0.f;
    for (const char *key : { "clipping_of_view", "cursor_size", "smart_fill_angle", "strength", "hardness", "shade", "gradient_from", "gradient_to" })
        sliders_left_width = std::max(sliders_left_width, m_imgui->calc_text_size(m_desc.at(key)).x + m_imgui->scaled(1.5f));
    sliders_left_width = std::max(sliders_left_width, m_imgui->calc_text_size(m_desc.at("reset_direction")).x + m_imgui->scaled(1.5f) +
                                                          ImGui::GetStyle().FramePadding.x * 2);
    const float empty_button_width = m_imgui->calc_button_size("").x;
    const float slider_icon_width  = m_imgui->get_slider_icon_size().x;
    const float max_tooltip_width  = ImGui::GetFontSize() * 20.0f;
    const float sliders_width      = m_imgui->scaled(7.0f);
    const float drag_left_width    = ImGui::GetStyle().WindowPadding.x + sliders_left_width + sliders_width - space_size;

    float caption_max = 0.f;
    for (const char *key : { "spray", "spray_opposite", "erase", "cursor_size", "clipping_of_view", "fill", "fill_erase", "gradient", "smart_fill_angle" })
        caption_max = std::max(caption_max, m_imgui->calc_text_size(m_desc.at(std::string(key) + "_caption")).x);
    caption_max += m_imgui->scaled(1.f);

    auto slider = [&](const char *key, const char *id, float *value, float v_min, float v_max, const char *format) {
        ImGui::AlignTextToFramePadding();
        m_imgui->text(m_desc.at(key));
        ImGui::SameLine(sliders_left_width);
        ImGui::PushItemWidth(sliders_width);
        bool changed = m_imgui->bbl_slider_float_style(std::string("##") + id, value, v_min, v_max, format, 1.0f, true);
        ImGui::SameLine(drag_left_width);
        ImGui::PushItemWidth(1.5 * slider_icon_width);
        changed |= ImGui::BBLDragFloat((std::string("##") + id + "_input").c_str(), value, 0.05f, 0.0f, 0.0f, format);
        *value = std::clamp(*value, v_min, v_max);
        return changed;
    };

    ImGui::AlignTextToFramePadding();
    m_imgui->text(m_desc.at("tool_type"));
    const std::array<wchar_t, 4>  tool_ids  = { ImGui::CircleButtonIcon, ImGui::SphereButtonIcon, ImGui::FillButtonIcon, ImGui::HeightRangeIcon };
    const std::array<wchar_t, 4>  icons     = m_is_dark_mode ? std::array<wchar_t, 4>{ ImGui::CircleButtonDarkIcon, ImGui::SphereButtonDarkIcon, ImGui::FillButtonDarkIcon, ImGui::HeightRangeDarkIcon } :
                                                               tool_ids;
    const std::array<wxString, 4> tool_tips = { _L("Airbrush (circle)"), _L("Airbrush (sphere)"), _L("Fill"), _L("Gradient") };
    for (size_t i = 0; i < tool_ids.size(); ++i) {
        const std::wstring btn_name = icons[i] + boost::nowide::widen(std::string("##airbrush_tool") + std::to_string(i));
        if (i != 0)
            ImGui::SameLine((empty_button_width + m_imgui->scaled(1.75f)) * i + m_imgui->scaled(1.3f));
        ImGui::PushStyleVar(ImGuiStyleVar_FrameBorderSize, 0.0);
        const bool selected = m_current_tool == tool_ids[i];
        if (selected) {
            const ImVec4 bg = m_is_dark_mode ? ImVec4(43 / 255.0f, 64 / 255.0f, 54 / 255.0f, 1.00f) : ImVec4(0.86f, 0.99f, 0.91f, 1.00f);
            ImGui::PushStyleColor(ImGuiCol_Button, bg);
            ImGui::PushStyleColor(ImGuiCol_ButtonHovered, bg);
            ImGui::PushStyleColor(ImGuiCol_ButtonActive, bg);
            ImGui::PushStyleColor(ImGuiCol_Border, ImVec4(0.00f, 0.68f, 0.26f, 1.00f));
            ImGui::PushStyleVar(ImGuiStyleVar_FrameBorderSize, 1.0);
            ImGui::PushStyleVar(ImGuiStyleVar_FrameRounding, 1.0);
        }
        const bool clicked = ImGui::Button(into_u8(btn_name).c_str());
        if (selected) {
            ImGui::PopStyleColor(4);
            ImGui::PopStyleVar(2);
        }
        ImGui::PopStyleVar(1);
        if (clicked && !selected) {
            m_current_tool = tool_ids[i];
            for (auto &triangle_selector : m_triangle_selectors) {
                triangle_selector->seed_fill_unselect_all_triangles();
                triangle_selector->request_update_render_data();
            }
        }
        if (ImGui::IsItemHovered())
            m_imgui->tooltip(tool_tips[i], max_tooltip_width);
    }
    ImGui::Dummy(ImVec2(0.0f, ImGui::GetFontSize() * 0.1));
    ImGui::Separator();

    if (m_current_tool == ImGui::CircleButtonIcon || m_current_tool == ImGui::SphereButtonIcon) {
        m_cursor_type = m_current_tool == ImGui::CircleButtonIcon ? TriangleSelector::CursorType::CIRCLE : TriangleSelector::CursorType::SPHERE;
        m_tool_type   = ToolType::BRUSH;

        if (ImGui::RadioButton(into_u8(m_desc.at("darker")).c_str(), m_darker))
            m_darker = true;
        ImGui::SameLine();
        if (ImGui::RadioButton(into_u8(m_desc.at("lighter")).c_str(), !m_darker))
            m_darker = false;

        slider("cursor_size", "cursor_radius", &m_cursor_radius, CursorRadiusMin, CursorRadiusMax, "%.2f");
        slider("strength", "strength", &m_strength, StrengthMin, StrengthMax, "%.2f");
        float hardness_percent = m_hardness * 100.f;
        if (slider("hardness", "hardness", &hardness_percent, 0.f, 90.f, "%.0f%%"))
            m_hardness = hardness_percent / 100.f;
    } else if (m_current_tool == ImGui::FillButtonIcon) {
        m_cursor_type = TriangleSelector::CursorType::POINTER;
        m_tool_type   = ToolType::SMART_FILL;
        float shade_percent = m_fill_shade * 100.f;
        if (slider("shade", "fill_shade", &shade_percent, -100.f, 100.f, "%.0f%%"))
            m_fill_shade = shade_percent / 100.f;
        if (slider("smart_fill_angle", "smart_fill_angle", &m_smart_fill_angle, SmartFillAngleMin, SmartFillAngleMax, "%.0f"))
            for (auto &triangle_selector : m_triangle_selectors) {
                triangle_selector->seed_fill_unselect_all_triangles();
                triangle_selector->request_update_render_data();
            }
    } else if (m_current_tool == ImGui::HeightRangeIcon) {
        // Smart fill highlights the area the gradient goes to while hovering.
        m_cursor_type = TriangleSelector::CursorType::POINTER;
        m_tool_type   = ToolType::SMART_FILL;
        float from_percent = m_gradient_from * 100.f;
        float to_percent   = m_gradient_to * 100.f;
        if (slider("gradient_from", "gradient_from", &from_percent, -100.f, 100.f, "%.0f%%"))
            m_gradient_from = from_percent / 100.f;
        if (slider("gradient_to", "gradient_to", &to_percent, -100.f, 100.f, "%.0f%%"))
            m_gradient_to = to_percent / 100.f;
        slider("smart_fill_angle", "smart_fill_angle", &m_smart_fill_angle, SmartFillAngleMin, SmartFillAngleMax, "%.0f");
        if (m_gradient.active) {
            ImDrawList *draw_list = ImGui::GetForegroundDrawList();
            draw_list->AddLine(ImVec2(float(m_gradient.start_screen.x()), float(m_gradient.start_screen.y())),
                               ImVec2(float(m_gradient.end_screen.x()), float(m_gradient.end_screen.y())), IM_COL32(0, 174, 66, 255), 2.f);
        }
    }
    m_imgui->text(_L("-100% = darkest, 100% = lightest"));

    ImGui::Separator();
    if (m_c->object_clipper()->get_position() == 0.f) {
        ImGui::AlignTextToFramePadding();
        m_imgui->text(m_desc.at("clipping_of_view"));
    } else if (m_imgui->button(m_desc.at("reset_direction"))) {
        wxGetApp().CallAfter([this]() { m_c->object_clipper()->set_position(-1., false); });
    }
    auto clp_dist = float(m_c->object_clipper()->get_position());
    ImGui::SameLine(sliders_left_width);
    ImGui::PushItemWidth(sliders_width);
    bool clp_changed = m_imgui->bbl_slider_float_style("##clp_dist", &clp_dist, 0.f, 1.f, "%.2f", 1.0f, true);
    ImGui::SameLine(drag_left_width);
    ImGui::PushItemWidth(1.5 * slider_icon_width);
    clp_changed |= ImGui::BBLDragFloat("##clp_dist_input", &clp_dist, 0.05f, 0.0f, 0.0f, "%.2f");
    if (clp_changed)
        m_c->object_clipper()->set_position(clp_dist, true);

    ImGui::Separator();
    render_shading_filaments_info();

    ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(6.0f, 10.0f));
    const float get_cur_y = ImGui::GetContentRegionMax().y + ImGui::GetFrameHeight() + y;
    show_tooltip_information(caption_max, x, get_cur_y);

    const float f_scale = m_parent.get_main_toolbar_scale();
    ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(6.0f, 4.0f * f_scale));
    ImGui::SameLine();
    if (m_imgui->button(m_desc.at("remove_all"))) {
        Plater::TakeSnapshot snapshot(wxGetApp().plater(), "Reset selection", UndoRedo::SnapshotType::GizmoAction);
        ModelObject *mo  = m_c->selection_info()->model_object();
        int          idx = -1;
        for (ModelVolume *mv : mo->volumes)
            if (mv->is_model_part()) {
                ++idx;
                m_triangle_selectors[idx]->reset();
                m_triangle_selectors[idx]->request_update_render_data(true);
            }
        update_model_object();
        m_parent.set_as_dirty();
    }
    ImGui::PopStyleVar(2);

    m_imgui_end_pos[0] = m_imgui_start_pos[0] + ImGui::GetWindowWidth();
    m_imgui_end_pos[1] = m_imgui_start_pos[1] + ImGui::GetWindowHeight();
    GizmoImguiEnd();
    ImGuiWrapper::pop_toolbar_style();
}

void GLGizmoAirbrush::show_tooltip_information(float caption_max, float x, float y)
{
    ImTextureID normal_id = m_parent.get_gizmos_manager().get_icon_texture_id(GLGizmosManager::MENU_ICON_NAME::IC_TOOLBAR_TOOLTIP);
    ImTextureID hover_id  = m_parent.get_gizmos_manager().get_icon_texture_id(GLGizmosManager::MENU_ICON_NAME::IC_TOOLBAR_TOOLTIP_HOVER);

    caption_max += m_imgui->calc_text_size(": ").x + 15.f;

    const float  font_size   = ImGui::GetFontSize();
    const ImVec2 button_size = ImVec2(font_size * 1.8, font_size * 1.3);
    ImGui::PushStyleVar(ImGuiStyleVar_FrameBorderSize, 0.0f);
    ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, { 0, ImGui::GetStyle().FramePadding.y });
    ImGui::ImageButton3(normal_id, hover_id, button_size);

    if (ImGui::IsItemHovered()) {
        ImGui::BeginTooltip2(ImVec2(x, y));
        std::vector<std::string> tip_items;
        if (m_current_tool == ImGui::FillButtonIcon)
            tip_items = { "fill", "fill_erase", "smart_fill_angle", "clipping_of_view" };
        else if (m_current_tool == ImGui::HeightRangeIcon)
            tip_items = { "gradient", "smart_fill_angle", "clipping_of_view" };
        else
            tip_items = { "spray", "spray_opposite", "erase", "cursor_size", "clipping_of_view" };
        for (const std::string &t : tip_items) {
            m_imgui->text_colored(ImGuiWrapper::COL_ACTIVE, m_desc.at(t + "_caption") + ": ");
            ImGui::SameLine(caption_max);
            m_imgui->text_colored(ImGuiWrapper::COL_WINDOW_BG, m_desc.at(t));
        }
        ImGui::EndTooltip();
    }
    ImGui::PopStyleVar(2);
}

void GLGizmoAirbrush::update_model_object()
{
    bool         updated = false;
    ModelObject *mo      = m_c->selection_info()->model_object();
    int          idx     = -1;
    for (ModelVolume *mv : mo->volumes) {
        if (!mv->is_model_part())
            continue;
        ++idx;
        updated |= mv->shade_facets.set(*m_triangle_selectors[idx].get());
    }
    if (updated) {
        const ModelObjectPtrs &mos = wxGetApp().model().objects;
        wxGetApp().obj_list()->update_info_items(std::find(mos.begin(), mos.end(), mo) - mos.begin());
        m_parent.post_event(SimpleEvent(EVT_GLCANVAS_SCHEDULE_BACKGROUND_PROCESS));
    }
}

std::vector<std::array<float, 4>> GLGizmoAirbrush::state_colors(const ModelVolume &mv) const
{
    std::array<float, 4> base = GLVolume::NEUTRAL_COLOR;
    const std::vector<std::array<float, 4>> filament_colors = wxGetApp().plater()->get_extruders_colors();
    const int filament = mv.extruder_id();
    if (filament >= 1 && filament <= int(filament_colors.size()))
        base = filament_colors[filament - 1];
    base[3] = 1.f;

    std::vector<std::array<float, 4>> colors(2 * Shading::kLevels + 1, base);
    for (int state = 1; state <= 2 * Shading::kLevels; ++state) {
        const int   level  = Shading::state_to_level(state);
        const float target = level < 0 ? 0.f : 1.f;
        // Even the weakest shade must be visible, the strongest is close to black or white.
        const float t = 0.12f + 0.8f * float(std::abs(level)) / Shading::kLevels;
        for (int c = 0; c < 3; ++c)
            colors[state][c] = base[c] + (target - base[c]) * t;
    }
    return colors;
}

void GLGizmoAirbrush::update_from_model_object(bool first_update)
{
    wxBusyCursor wait;

    const ModelObject *mo = m_c->selection_info()->model_object();
    m_triangle_selectors.clear();
    for (const ModelVolume *mv : mo->volumes) {
        if (!mv->is_model_part())
            continue;
        m_triangle_selectors.emplace_back(std::make_unique<TriangleSelectorPatch>(mv->mesh(), state_colors(*mv), 0.2f));
        // Reset of TriangleSelector is done inside TriangleSelectorGUI's constructor, so we don't need it to perform it again in deserialize().
        m_triangle_selectors.back()->deserialize(mv->shade_facets.get_data(), false);
        m_triangle_selectors.back()->request_update_render_data();
        m_triangle_selectors.back()->set_wireframe_needed(true);
    }
}

wxString GLGizmoAirbrush::handle_snapshot_action_name(bool shift_down, GLGizmoPainterBase::Button button_down) const
{
    if (shift_down)
        return "Erase shading";
    if (m_tool_type == ToolType::SMART_FILL)
        return button_down == Button::Left ? "Fill shading" : "Erase shading";
    return (button_down == Button::Left) == m_darker ? "Spray darker" : "Spray lighter";
}

} // namespace Slic3r::GUI

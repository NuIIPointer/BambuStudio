#ifndef slic3r_GLGizmoAirbrush_hpp_
#define slic3r_GLGizmoAirbrush_hpp_

#include "GLGizmoPainterBase.hpp"

#include <map>
#include <random>

namespace Slic3r::GUI {

// Airbrush painting for shading: sprays darker or lighter shades with a soft brush. The shade is printed by
// tinting the inner walls behind the outer wall with a dark or light filament (see libslic3r/Shading.hpp).
class GLGizmoAirbrush : public GLGizmoPainterBase
{
public:
    GLGizmoAirbrush(GLCanvas3D &parent, unsigned int sprite_id);

    void data_changed(bool is_serializing) override;
    void render_painter_gizmo() const override;
    bool gizmo_event(SLAGizmoEventType action, const Vec2d &mouse_position, bool shift_down, bool alt_down, bool control_down) override;

    bool        on_key_down_select_tool_type(int keyCode);
    std::string get_icon_filename(bool is_dark_mode) const override;

protected:
    void        on_render_input_window(float x, float y, float bottom_limit) override;
    std::string on_get_name() const override;
    std::string on_get_name_str() override { return "Airbrush Shading"; }

    void     render_triangles(const Selection &selection) const override;
    void     on_set_state() override;
    wxString handle_snapshot_action_name(bool shift_down, Button button_down) const override;

    std::string get_gizmo_entering_text() const override { return "Entering airbrush shading"; }
    std::string get_gizmo_leaving_text() const override { return "Leaving airbrush shading"; }
    std::string get_action_snapshot_name() override { return "Airbrush shading editing"; }

    // Used by the fill tool: left sets the chosen shade, right erases.
    EnforcerBlockerType get_left_button_state_type() const override;
    EnforcerBlockerType get_right_button_state_type() const override { return EnforcerBlockerType::NONE; }

    bool paint_brush_stroke(TriangleSelectorGUI                       &selector,
                            const std::vector<ProjectedMousePosition> &positions,
                            const Vec3f                               &camera_pos,
                            const Transform3d                         &trafo_matrix,
                            const TriangleSelector::ClippingPlane     &clp,
                            Button                                     button,
                            bool                                       shift_down) override;

private:
    bool on_init() override;
    void update_model_object() override;
    void update_from_model_object(bool first_update) override;
    void on_opening() override {}
    void on_shutdown() override;
    PainterGizmoType get_painter_type() const override { return PainterGizmoType::AIRBRUSH; }

    void show_tooltip_information(float caption_max, float x, float y);
    void render_shading_filaments_info();
    // Colors of the 33 painted states for one part: its filament color tinted towards black or white.
    std::vector<std::array<float, 4>> state_colors(const ModelVolume &mv) const;

    // Raycast of the mouse into the selected object, returns the mesh index or -1.
    int  raycast(const Vec2d &mouse_position, Vec3f &hit, size_t &facet, Transform3d &trafo) const;
    void apply_gradient();

    bool  m_darker      = true;
    float m_strength    = 2.f;   // shade levels per spray at the brush center
    float m_hardness    = 0.3f;  // share of the brush radius without falloff
    float m_fill_shade  = -0.5f; // -1 = darkest, 1 = lightest
    float m_gradient_from = -1.f;
    float m_gradient_to   = 0.f;

    struct GradientDrag
    {
        bool        active = false;
        int         mesh_idx = -1;
        size_t      facet = 0;
        Vec3f       start_mesh = Vec3f::Zero();
        Vec3f       end_mesh = Vec3f::Zero();
        Transform3d trafo = Transform3d::Identity();
        Vec2d       start_screen = Vec2d::Zero();
        Vec2d       end_screen = Vec2d::Zero();
    };
    GradientDrag m_gradient;

    std::mt19937 m_rng{ 5489u };
    std::map<std::string, wxString> m_desc;
};

} // namespace Slic3r::GUI

#endif // slic3r_GLGizmoAirbrush_hpp_

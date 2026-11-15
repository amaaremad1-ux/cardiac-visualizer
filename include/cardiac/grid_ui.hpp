// grid_ui.hpp - ImGui controls and integration for the MultiSubjectGrid.
// Provides the control panel (subject selector, time duration, live toggle) and
// the per-frame invocation of the grid viewports.

#pragma once

#include "cardiac/grid_viewport.hpp"
#include "cardiac/database_manager.hpp"
#include "cardiac/types.hpp"
#include "dashboard_draw.hpp"
#include "imgui.h"
#include <GLFW/glfw3.h>
#include <cstdint>
#include <string>
#include <vector>

namespace cardiac::ui {

// Per-frame UI controls for the multi-subject grid view. Called by
// dashboard_frame() each frame. Draws the subject selector, time-period
// slider, live switch, and renders the grid viewport cells.
void draw_grid_control_panel(GLFWwindow* w,
                             MultiSubjectGrid& grid,
                             DatabaseManager& db,
                             const std::vector<SessionRecord>& history,
                             const std::string& title,
                             int width, int height,
                             const std::string& msg);

// Internal: populates the subject list dropdown from stored sessions.
void populate_subject_list(ImGuiTextFilter* filter, const std::vector<SessionRecord>& history,
                           std::vector<std::string>& out_ids);

}  // namespace cardiac::ui
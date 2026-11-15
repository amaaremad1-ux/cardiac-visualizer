#pragma once
#include "cardiac/dashboard.hpp"
#include "cardiac/device_adapter.hpp"
#include <memory>
namespace cardiac::ui {
struct Camera;
// Builds the dashboard session: live BLE (idle NullBle transport until the
// real WinRT transport plugs in) or labeled synthetic demo stream.
void build_dashboard_session(const DashboardOptions& options,
                             std::shared_ptr<SessionController>& session,
                             std::shared_ptr<DeviceAdapter>& adapter);
}  // namespace cardiac::ui

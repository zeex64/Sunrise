#include "server_ui_module_runtime.h"

#include <string_view>

#include "../../../core/ui/modules/registry/ui_module_registry.h"
#include "../../../core/ui/modules/ui_module_descriptor.h"
#include "../activity_override/activity_override_panel.h"
#include "../squads/squad_panel.h"

namespace sunrise::server::ui::runtime {
namespace {

/** A namespaced stable ID keeps Server modules from clashing with Client modules. */
constexpr std::string_view kOverrideStableId = "server.activity_override";
constexpr std::string_view kSquadsStableId = "server.squads";
/** Short menu label for the activity override page. */
constexpr std::string_view kOverrideDisplayName = "Activity";
/** Short menu label for the authored squad placement page. */
constexpr std::string_view kSquadsDisplayName = "Squads";

core::ui::modules::registry::PageRegistration g_overridePage;
core::ui::modules::registry::PageRegistration g_squadsPage;

} // namespace

/** @return True when both Server modules own their Core UI registry slots. */
bool initialize() noexcept {
    const bool overrideOwned = g_overridePage.acquire(core::ui::modules::Owner::server,
                                                      kOverrideStableId,
                                                      kOverrideDisplayName,
                                                      &activity_override::draw);
    // Registered after Activity, which is the order the menu lists the Server pages in.
    const bool squadsOwned = g_squadsPage.acquire(
        core::ui::modules::Owner::server, kSquadsStableId, kSquadsDisplayName, &squads::draw);
    if (overrideOwned && squadsOwned) {
        return true;
    }
    g_squadsPage.release();
    g_overridePage.release();
    return false;
}

/** Removes the Server modules from the Core UI registry. */
void shutdown() noexcept {
    g_squadsPage.release();
    g_overridePage.release();
}

} // namespace sunrise::server::ui::runtime

#pragma once
// -----------------------------------------------------------------------------
// navmap_auditor.h — F10 ImGui tool for auditing every system-map layout.
// -----------------------------------------------------------------------------

#include "sokol_app.h"

namespace navmap_auditor {

void init();
bool handle_event(const sapp_event* e);
void build();

} // namespace navmap_auditor

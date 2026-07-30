#pragma once

struct BaseContext;

namespace commodity_ui {

// Responsive Commodity Exchange presentation. Transactions continue through
// the economy/player models; no room-art hotspot coordinates are consulted.
void draw_exchange(BaseContext& ctx);

} // namespace commodity_ui

#pragma once
struct sapp_event;
namespace base_art_studio {
void init();
void build();
bool handle_event(const sapp_event* event);
void shutdown();
}

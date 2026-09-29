#pragma once
// hit_facing.h — which armor/shield quadrant a hit lands on. Top-level and
// standalone so collision code (projectile.cpp), ship code (ship.cpp) and the
// component-damage model (ship_systems.h) share one definition.

#include <cstdint>

enum class HitFacing : uint8_t { Fore, Aft, Port, Starboard };

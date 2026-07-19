#pragma once
// -----------------------------------------------------------------------------
// world_clock.h — pure Confed-stardate helpers for the persistent world.
//
// The mutable day counter belongs to PlayerState. Keeping formatting and
// quarter math pure makes schedules, UI, and headless tests independent of
// the game host and save system.
// -----------------------------------------------------------------------------

#include <string>

namespace world_clock {

constexpr int k_epoch_year = 2669;
constexpr int k_epoch_doy = 135;
constexpr int k_concordia_news_day = 108;

// Format elapsed world days as the canonical Wing Commander YYYY.DDD date.
// Negative input is treated as day zero; persisted state never needs it.
std::string stardate_string(int day);

// Economic quarter in the range 1..4, using 91-day blocks from the epoch.
// Q4 owns the year's remaining days. Negative input is treated as day zero.
int quarter(int day);

} // namespace world_clock

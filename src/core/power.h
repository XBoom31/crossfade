#pragma once

namespace power
{
// Re-read the battery state at most once every 30 s. Safe to call from the
// main loop every iteration.
void refresh();

// True when any system battery reports "Discharging". Always false on
// platforms without /sys/class/power_supply (including Windows).
bool on_battery();

// Manual toggle from Settings (low_power) and the "enable on battery"
// companion option. Called by apply_ui_scale() whenever settings change.
void configure(bool manual, bool auto_on_battery);

// True when P100_LOW_POWER is set to a truthy value. The environment wins
// over both the manual toggle and the battery check.
bool env_forced();

// Session-only override for the --low-power command-line flag. Never written
// back to settings.json.
void set_cli_override(bool on);

// Effective state: env / CLI override, manual toggle, or auto-on-battery.
bool low_power();
} // namespace power

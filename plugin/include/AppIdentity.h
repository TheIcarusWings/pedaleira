#pragma once

// Per-user data folder name (settings, presets, local models, logs live under
// <user app-data root>/<this>). Differs from upstream's "TONE3000" so Pedaleira
// can be installed next to the official plugin without sharing state.
inline constexpr const char* kAppDataFolderName = "Pedaleira";

#pragma once

// Per-user data folder name (settings, presets, local models, logs live under
// <user app-data root>/<this>). Differs from upstream's "TONE3000" so Pedaleira
// can be installed next to the official plugin without sharing state.
inline constexpr const char* kAppDataFolderName = "Pedaleira";

// The test build's label ("build 38, 2026-10-19"), set by the package
// workflow through PEDALEIRA_BUILD_LABEL so two builds of one version can be
// told apart. Empty for local builds.
#if defined(PEDALEIRA_BUILD_LABEL)
inline constexpr const char* kBuildLabel = PEDALEIRA_BUILD_LABEL;
#else
inline constexpr const char* kBuildLabel = "";
#endif

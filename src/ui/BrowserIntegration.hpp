#pragma once

#include <QString>

// Registers orbit-native-host.exe as a native messaging host for Chromium-based browsers
// (Chrome, Edge, Brave, Chromium) and Firefox, so the Orbit extension can hand downloads
// over with their cookies and referrer and hear back whether Orbit took them.
namespace browser_integration {

inline constexpr const char* kNativeHostName = "com.orbit.downloadmanager";
// Fixed by the "key" in the extension's manifest.json, so unpacked installs keep this ID.
inline constexpr const char* kChromeExtensionId = "ocipkapjmoolheihhoedndhefjpnbccg";
inline constexpr const char* kFirefoxExtensionId = "orbit-extension@orbit.app";

[[nodiscard]] QString nativeHostExecutable();

// Writes the host manifests and registry entries for the current user. Returns an
// error message, or an empty string on success.
QString registerNativeMessagingHost();
void unregisterNativeMessagingHost();
[[nodiscard]] bool isNativeMessagingHostRegistered();

}

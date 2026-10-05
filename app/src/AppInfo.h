#pragma once
// The application's name and version, and where it keeps its settings.

#include <QString>

namespace sub::app {

inline constexpr const char* kAppName = "SUBstation";
inline constexpr const char* kOrganization = "SUBstation";

QString version();

}  // namespace sub::app

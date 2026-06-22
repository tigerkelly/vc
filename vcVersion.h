#ifndef VC_VERSION_H
#define VC_VERSION_H

// Single source of truth for vc, vcd, and vcg version numbers.
// Bump this when releasing a new version.

#define APP_VERSION     "1.0"

// Build date/time — set by the compiler at compile time.
#define VC_BUILD_DATE   __DATE__
#define VC_BUILD_TIME   __TIME__

#endif // VC_VERSION_H

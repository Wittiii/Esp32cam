#pragma once

// Shared camera/RTSP sources also build for ESP32-CAM. Only DFR builds record
// these markers; the other profiles keep their original execution path.
#ifdef DFR_CAMERA_DIAGNOSTICS
#include "dfr1154_diagnostics.h"
#define DFR_DIAGNOSTIC_SCOPE(stage) dfrdiag::Scope dfrDiagnosticScope(stage)
#else
#define DFR_DIAGNOSTIC_SCOPE(stage) ((void)0)
#endif

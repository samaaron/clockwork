// SPDX-License-Identifier: AGPL-3.0-or-later OR LicenseRef-Clockwork-Commercial
// Copyright (c) 2025 Sam Aaron
/*
 * MicPermission.mm — Apple microphone permission check via AVFoundation
 */
#ifdef __APPLE__

#include "MicPermission.h"
#import <AVFoundation/AVFoundation.h>
#import <Foundation/Foundation.h>
#include <TargetConditionals.h>
#include <unistd.h>
#include "clockwork_config.h"   // clockwork_log

#if TARGET_OS_OSX
#include <libproc.h>

// Private API from libsystem_secinit. Given a PID, returns the PID macOS
// considers "responsible" for it (for TCC / LaunchServices attribution).
// Returns 0 on error, or the input pid if no responsible-process mapping.
// macOS only: iOS has no responsible process to ask about, and a private
// symbol in an iOS binary is an App Store rejection.
extern "C" pid_t responsibility_get_pid_responsible_for_pid(pid_t);
#endif

namespace MicPermission {

std::string status() {
    AVAuthorizationStatus s =
        [AVCaptureDevice authorizationStatusForMediaType:AVMediaTypeAudio];
    switch (s) {
        case AVAuthorizationStatusNotDetermined: return "notDetermined";
        case AVAuthorizationStatusRestricted:    return "restricted";
        case AVAuthorizationStatusDenied:        return "denied";
        case AVAuthorizationStatusAuthorized:    return "authorized";
        default:                                 return "unknown";
    }
}

void logDiagnostics(ClockworkCore* core) {
    pid_t self = getpid();

    NSBundle* main = [NSBundle mainBundle];
    const char* bundleID = [[main bundleIdentifier] UTF8String];
    const char* bundlePath = [[main bundlePath] UTF8String];

    // Always-on: mic-permission diagnostics at boot are essential for
    // triaging "live_audio is silent" reports. A few lines once per process,
    // negligible overhead, invaluable in user bug reports.
#if TARGET_OS_OSX
    char selfPath[PROC_PIDPATHINFO_MAXSIZE] = {0};
    proc_pidpath(self, selfPath, sizeof(selfPath));

    pid_t respPid = responsibility_get_pid_responsible_for_pid(self);
    char respPath[PROC_PIDPATHINFO_MAXSIZE] = {0};
    if (respPid > 0) proc_pidpath(respPid, respPath, sizeof(respPath));

    clockwork_log(core, "[tcc-diag] self.pid=%d self.path=%s", self, selfPath);
#else
    clockwork_log(core, "[tcc-diag] self.pid=%d", self);
#endif
    clockwork_log(core, "[tcc-diag] self.bundleID=%s self.bundlePath=%s",
            bundleID ? bundleID : "(nil)", bundlePath ? bundlePath : "(nil)");
#if TARGET_OS_OSX
    clockwork_log(core, "[tcc-diag] responsible.pid=%d responsible.path=%s",
            respPid, respPath[0] ? respPath : "(unknown)");
#endif
    clockwork_log(core, "[tcc-diag] mic.status=%s", status().c_str());
}

} // namespace MicPermission

#endif

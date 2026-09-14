// cyclomp — control for the Android ride foreground service (RideService.java).
// The service keeps the process alive and lifts background-location throttling
// while a ride records; it does not own the GPS. Started as the ride leaves
// Idle, stopped on TERMINATE. See docs/HANDOVER-background-recording.md.
#pragma once
#ifdef __ANDROID__
namespace ridesvc {

// Bring up / tear down the foreground service. Both are idempotent — a second
// start is a no-op on the framework side, a stop when nothing runs is harmless.
void start();
void stop();

// Ask for POST_NOTIFICATIONS (runtime permission on API 33+; auto-granted, so
// a no-op, below that). As everywhere in this app, NativeActivity never
// forwards onRequestPermissionsResult, so the answer only shows up as a
// checkSelfPermission flip on a later call. Request codes: 1=location,
// 2=camera, 3=notifications.
void request_notification_permission();

} // namespace ridesvc
#endif

// SPDX-License-Identifier: GPL-2.0-or-later
#ifndef KYTY_LOADER_APPLICATION_SERVICE_RETURNING_EXPORTS_H_
#define KYTY_LOADER_APPLICATION_SERVICE_RETURNING_EXPORTS_H_

namespace Loader {

struct ApplicationServiceReturningExport {
	const char* nid;
	const char* expression;
};

// Reviewed production identities, not resolver aliases or a service-family
// allowlist. Match BOTH fields after the existing owner/version/address checks.
// Only normal baseline-SysV returns are classified. This does not enable shared
// launch, prove arbitrary-pointer safety, or make resource/service waits stop.
// No entry invokes a guest callback. Scope, policy limits and explicit startup
// blockers: docs/vsh-app-service-transitions-20261105.md.
inline constexpr ApplicationServiceReturningExport ApplicationServiceReturningExports[] {
	// libShellCoreFloor.cpp: actual PID-scoped metadata/authority queries backed
	// by runtimeProcessIdentity.cpp, not the neighboring generic floor stubs.
	{"G-MYv5erXaU", "KernelGetAppInfo"},
	{"igMefp4SAv0", "KGetAuthInfo"},

	// libAppContent.cpp: read the caller's immutable SystemContent snapshot.
	// Global mounts remain outside this returning-service slice.
	{"99b82IKXpH4", "AppContent::AppContentAppParamGetInt"},
	// Owner-scoped initialization/event queue; explicitly prepared before entry
	// and released after stop plus actual drain by runtimeVshLaunch.cpp.
	{"R9lA82OraNs", "AppContent::AppContentInitialize"},
	{"656LMQSrg6U", "SystemService::SystemServiceReceiveEvent"},
	// Prepared identity snapshots and independent per-lifetime event cursors.
	{"j3YMu1MVNNo", "UserService::UserServiceInitialize"},
	{"az-0R6eviZ0", "UserService::UserServiceInitialize2"},
	{"CdWp0oHWGr0", "UserService::UserServiceGetInitialUser"},
	{"yH17Q6NWtVg", "UserService::UserServiceGetEvent"},
	{"fPhymKNvK-A", "UserService::UserServiceGetLoginUserIdList"},
	{"1xxcMiGu2fo", "UserService::UserServiceGetUserName"},

	// libSystemService.cpp: typed local policy/configuration and non-consuming
	// status queries. ReceiveEvent above now uses the owner's private queue.
	{"fZo48un7LK4", "SystemService::SystemServiceParamGetInt"},
	{"SsC-m-S9JTA", "SystemService::SystemServiceParamGetString"},
	{"rPo6tV8D9bM", "SystemService::SystemServiceGetStatus"},
	{"1n37q1Bvc5Y", "SystemService::SystemServiceGetDisplaySafeAreaInfo"},
	// Literal registration expressions are unqualified inside DeviceAuth's
	// registration scope. Signing returns the native no-backend INTERNAL error;
	// it never manufactures a digest or claims a successful signing operation.
	{"KgiwSrROsIQ", "GetCertificationScope"},
	{"Oxh0CSsTZ78", "SignCertificationMessage"},

	// libVideoOut.cpp registrations: pure caller-buffer construction/decoding
	// in presentation/videoOut.cpp and videoOutStatus.cpp. No driver/port mutation,
	// display wait, GPU submission, guest event dispatch or media-family admission.
	// SetBufferAttribute2 has eight scalar/pointer arguments, including stack args.
	{"PjS5uASwcV8", "VideoOut::VideoOutSetBufferAttribute2"},
	{"+I4K03i3EL0", "VideoOut::VideoOutInitializeOutputOptions"},
	{"d7eh3psk1J8", "VideoOut::VideoOutPortStatusIsUsable"},
	{"q9HFykGcuWU", "VideoOut::VideoOutPortStatusIsDeviceConnected"},
	{"v9gmbZSAovI", "VideoOut::VideoOutOutputStatusIsHdcp14Enabled"},
	{"ys239q8VgXY", "VideoOut::VideoOutOutputStatusIsHdcp23Enabled"},

	// libPad.cpp registrations / controller.cpp bodies: idempotent initialization
	// cannot reset an open shell port.
	// No PadOpen/Close, report consumption or hardware-output mutation is admitted.
	{"hv1luiJrqQM", "Controller::PadInit"},
	// PadGetControllerInformation stays excluded: a valid VR handle currently
	// passes admission but has no standard-player mapping in that implementation.
};

} // namespace Loader
#endif

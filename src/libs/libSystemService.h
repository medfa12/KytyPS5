// SPDX-License-Identifier: GPL-2.0-or-later
#ifndef KYTY_LIBS_LIB_SYSTEM_SERVICE_H_
#define KYTY_LIBS_LIB_SYSTEM_SERVICE_H_

#include "libs/systemService.h"

#include <cstdint>
#include <memory>

namespace Loader {
class RuntimeGuestExecution;
class SymbolDatabase;
}

namespace Libs {
void InitSystemService_1(Loader::SymbolDatabase* symbols);

namespace SystemService {
struct SystemServiceStatus {
	int32_t event_num                  = 0;
	bool    is_system_ui_overlaid      = false;
	bool    is_in_background_execution = false;
	bool    is_vr_play_area_overlaid   = false;
	uint8_t reserved[127]              = {};
};

struct SystemServiceEvent {
	int32_t event_type;
	uint8_t data[8192];
};
static_assert(sizeof(SystemServiceStatus) == 136);
static_assert(sizeof(SystemServiceEvent) == 8196);

// HOST lifecycle: register the immutable execution snapshot before any owned
// initializer/entry can call services. Re-registration is idempotent and never
// clears pending events. No subscription is created implicitly by guest queries.
bool RegisterApplicationEvents(const std::shared_ptr<Loader::RuntimeGuestExecution>& execution);
// Call with the SAME snapshot after stop + actual owner drain (including failed
// startup). Refuses running/outstanding work. Idempotent; cannot retire a newer
// lifetime even when its Program address is reused. Never touches session events.
bool ReleaseApplicationEvents(const std::shared_ptr<Loader::RuntimeGuestExecution>& execution);
// Source-scoped initialization/update hint, not an entitlement grant or a global
// broadcast. A captured producer cannot accidentally deliver to a relaunched app.
bool NotifyApplicationEntitlementUpdate(const std::shared_ptr<Loader::RuntimeGuestExecution>& execution);
} // namespace SystemService
} // namespace Libs
#endif

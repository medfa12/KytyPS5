// SPDX-License-Identifier: GPL-2.0-or-later
#ifndef KYTY_LIBS_LIB_APP_CONTENT_H_
#define KYTY_LIBS_LIB_APP_CONTENT_H_

#include <cstdint>

namespace Loader { class SymbolDatabase; }
namespace Libs {
void InitAppContent_1(Loader::SymbolDatabase* symbols);
namespace AppContent {
struct AppContentInitParam {
	char reserved[32];
};
struct AppContentBootParam {
	char     reserved1[4];
	uint32_t attr;
	char     reserved2[32];
};
static_assert(sizeof(AppContentInitParam) == 32);
static_assert(sizeof(AppContentBootParam) == 40);
} // namespace AppContent
} // namespace Libs
#endif

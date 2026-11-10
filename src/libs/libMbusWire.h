#ifndef EMULATOR_INCLUDE_EMULATOR_LIBS_LIBMBUSWIRE_H_
#define EMULATOR_INCLUDE_EMULATOR_LIBS_LIBMBUSWIRE_H_

#include <cstdint>

namespace Libs::MbusWire::MbusWireHost {

// Host-injectable Mbus event (see libMbusWire.cpp). Queued for the wiring
// turn that drains it ahead of the scripted SceMbusEventReceive schedule.
void InjectMbusEvent(uint32_t id);

} // namespace Libs::MbusWire::MbusWireHost

#endif /* EMULATOR_INCLUDE_EMULATOR_LIBS_LIBMBUSWIRE_H_ */

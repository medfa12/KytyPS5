#ifndef KYTY_LIBS_LIB_MBUS_H_
#define KYTY_LIBS_LIB_MBUS_H_

// PORT (kytyps5, 13.60 FW lineage): ported from voldemort-ref (4.03 FW lineage) libMbus.h.
// His file documents the dead-end verdict on SceMbus (libSceMbus.sprx, NIDs n7I71nQgO3M,
// z4xxDvors0k, 3a5PMZh-iP8): not a transport for ShellUI but an audio-hardware abstraction
// with a "server"-"client" pair, and SCE's own error-string tables already decode the bus
// errors. No code to port: mbus NIDs are deliberately NOT bound (the -5 crash earlier came
// from treating kernel transport NIDs as guest-callable by treating a -5 return as success;
// that same crash class is why n7I71nQgO3M stays a loader symbol only).
// Plain header, intentionally no code: included so the verdict is searchable and the
// mistake is not re-attempted.

namespace Libs::Mbus {
// Verdict: SceMbus = audio-hardware abstraction ("server"-"client"), NOT ShellUI transport.
// NIDs n7I71nQgO3M / z4xxDvors0k / 3a5PMZh-iP8 stay UNBOUND as guest functions.
// n7I71nQgO3M appears only as a loader symbol (monosgen bridge); calling it as a
// function pointer reproduces the -5-as-pointer crash class at 0xfea5.
}

#endif

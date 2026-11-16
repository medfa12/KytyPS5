#include "kernel/eventQueue.h"

#include "common/assert.h"
#include "common/common.h"
#include "common/logging/log.h"
#include "common/stringUtils.h"
#include "common/threads.h"
#include "common/timer.h"
#include "kernel/fileSystem.h"
#include "kernel/guestDescriptorRange.h"
#include "libs/errno.h"
#include "libs/libs.h"
#include "libs/network.h"

#include <algorithm>
#include <chrono>
#include <fmt/format.h>
#include <limits>
#include <list>
#include <new>
#include <unordered_map>
#include <vector>

namespace Libs::LibKernel::EventQueue {

LIB_NAME("libkernel", "libkernel");

constexpr uint16_t EV_ADD     = 0x01;
constexpr uint16_t EV_ONESHOT = 0x10;
constexpr uint16_t EV_CLEAR   = 0x20;
constexpr uint16_t EV_ERROR   = 0x4000;

struct KernelWriteEventWatch {
	std::shared_ptr<FileSystem::File> file;
};

static int PollWriteEvent(KernelEqueueEvent* event) {
	const int result = FileSystem::PollWriteEventFile(event->write_watch->file);
	event->triggered = result == OK;
	event->event.flags = EV_ADD;
	event->event.fflags = 0;
	event->event.data = 0;
	// Native generic VFS write filter explicitly reports ready/data=0, even
	// for read-only file descriptors. Zero is NOT an unknown-capacity sentinel.
	// Sockets/pipes cannot reach this backend; see the run19 kernel audit.
	return result;
}

static uint64_t MonotonicTimeNs() {
	return static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(
	                                 std::chrono::steady_clock::now().time_since_epoch())
	                                 .count());
}

static std::unordered_map<KernelEqueue, KernelEqueueRef> g_equeues;
static Common::Mutex                                     g_equeues_mutex;
static uint64_t                                          g_next_equeue = 1;

class KernelEqueuePrivate {
public:
	explicit KernelEqueuePrivate(KernelEqueue handle): m_handle(handle) {}
	virtual ~KernelEqueuePrivate();

	KYTY_CLASS_NO_COPY(KernelEqueuePrivate);

	[[nodiscard]] const std::string& GetName() const { return m_name; }
	void                             SetName(const std::string& m_name) { this->m_name = m_name; }

	int AddEvent(const KernelEqueueEvent& event);
	int TriggerEvent(uintptr_t ident, int16_t filter, void* trigger_data);
	int DeleteEvent(uintptr_t ident, int16_t filter);

	int  GetTriggeredEvents(KernelEvent* ev, int num);
	int  WaitForEvents(KernelEvent* ev, int num, uint32_t micros);
	void Close();

private:
	void TriggerExpiredTimers(uint64_t now_ns);
	bool GetNextTimerWaitMicros(uint64_t now_ns, uint32_t* wait_micros) const;

	std::list<KernelEqueueEvent> m_events;
	Common::Mutex                m_mutex;
	Common::CondVar              m_cond_var;
	std::string                  m_name;
	KernelEqueue                 m_handle = KERNEL_EQUEUE_INVALID;
	bool                         m_closed = false;
};

KernelEqueuePrivate::~KernelEqueuePrivate() {
	Close();
}

void KernelEqueuePrivate::Close() {
	Common::LockGuard lock(m_mutex);

	if (m_closed) {
		return;
	}
	m_closed = true;
	for (auto& event: m_events) {
		if (event.filter.delete_event_func != nullptr) {
			auto owner = event.filter.owner;
			event.filter.delete_event_func(m_handle, &event);
		}
	}
	m_events.clear();
	m_cond_var.SignalAll();
}

int KernelEqueuePrivate::GetTriggeredEvents(KernelEvent* ev, int num) {
	Common::LockGuard lock(m_mutex);

	EXIT_IF(num < 1);
	if (m_closed) {
		return KERNEL_ERROR_EBADF;
	}

	TriggerExpiredTimers(MonotonicTimeNs());
	for (auto it = m_events.begin(); it != m_events.end();) {
		if (it->write_watch && PollWriteEvent(&*it) == KERNEL_ERROR_EBADF) {
			// Like descriptor close in kqueue, retire the old identity; never
			// reinterpret its numeric fd as a newly allocated descriptor.
			it = m_events.erase(it);
		} else {
			++it;
		}
	}
	// Socket read registrations are level-triggered by readiness (data, pending
	// connection, or peer shutdown), checked without blocking on each wait pass.
	for (auto& event: m_events) {
		if (!event.triggered && event.event.filter == KERNEL_EVFILT_READ &&
		    Libs::Network::Net::IsSocketReadReady(static_cast<int>(event.event.ident))) {
			event.triggered  = true;
			event.event.data = 1;
		}
	}

	int ret = 0;

	for (auto it = m_events.begin(); it != m_events.end();) {
		auto& event = *it;
		bool  erase = false;
		while (event.triggered) {
			ev[ret++] = event.event;
			if ((event.event.flags & EV_ONESHOT) != 0) {
				erase = true;
				break;
			}

			if (event.filter.reset_func != nullptr) {
				event.filter.reset_func(&event);
			} else if ((event.event.flags & EV_CLEAR) != 0) {
				event.triggered    = false;
				event.event.fflags = 0;
				event.event.data   = 0;
			}

			if (!event.pending_events.empty()) {
				event.event = event.pending_events.front();
				event.pending_events.pop_front();
				event.triggered = true;
			} else {
				// A persistent level is delivered again by the next wait, not this batch.
				break;
			}

			if (ret >= num) {
				break;
			}
		}
		it = (erase ? m_events.erase(it) : std::next(it));
		if (ret >= num) {
			break;
		}
	}

	return ret;
}

void KernelEqueuePrivate::TriggerExpiredTimers(uint64_t now_ns) {
	for (auto& event: m_events) {
		if (event.deadline_ns != 0 && event.deadline_ns <= now_ns) {
			if (event.event.filter == KERNEL_EVFILT_TIMER) {
				const auto count = event.interval_ns == 0 ? !event.triggered :
				    1 + (now_ns - event.deadline_ns) / event.interval_ns;
				event.event.data += static_cast<intptr_t>(count);
				event.deadline_ns += count * event.interval_ns;
			}
			event.triggered = true;
		}
	}
}

bool KernelEqueuePrivate::GetNextTimerWaitMicros(uint64_t now_ns, uint32_t* wait_micros) const {
	EXIT_IF(wait_micros == nullptr);

	uint64_t nearest_deadline = UINT64_MAX;
	for (const auto& event: m_events) {
		// No host notification exists for socket readiness; re-poll every 10 ms.
		if (event.event.filter == KERNEL_EVFILT_READ &&
		    Libs::Network::Net::IsSocket(static_cast<int>(event.event.ident))) {
			nearest_deadline = std::min(nearest_deadline, now_ns + uint64_t {10000000});
		}
		if (!event.triggered && event.deadline_ns != 0) {
			nearest_deadline = std::min(nearest_deadline, event.deadline_ns);
		}
	}
	if (nearest_deadline == UINT64_MAX) {
		return false;
	}

	const auto remaining_ns = nearest_deadline > now_ns ? nearest_deadline - now_ns : 0;
	const auto rounded_us   = std::max<uint64_t>(1, (remaining_ns + 999u) / 1000u);
	*wait_micros            = static_cast<uint32_t>(std::min<uint64_t>(rounded_us, UINT32_MAX));
	return true;
}

int KernelEqueuePrivate::WaitForEvents(KernelEvent* ev, int num, uint32_t micros) {
	Common::LockGuard lock(m_mutex);

	EXIT_IF(num < 1);
	if (m_closed) {
		return KERNEL_ERROR_EBADF;
	}

	uint32_t      elapsed = 0;
	Common::Timer t;
	t.Start();

	for (;;) {
		int ret = GetTriggeredEvents(ev, num);

		if (ret != 0 || (elapsed >= micros && micros != 0)) {
			return ret;
		}

		uint32_t   timer_wait = 0;
		const bool has_timer  = GetNextTimerWaitMicros(MonotonicTimeNs(), &timer_wait);
		if (micros == 0 && !has_timer) {
			m_cond_var.Wait(&m_mutex);
		} else {
			const auto external_wait = micros != 0 ? micros - elapsed : UINT32_MAX;
			m_cond_var.WaitFor(&m_mutex,
			                   has_timer ? std::min(external_wait, timer_wait) : external_wait);
		}

		elapsed = static_cast<uint32_t>(t.GetTimeS() * 1000000.0);
	}

	return 0;
}

int KernelEqueuePrivate::AddEvent(const KernelEqueueEvent& event) {
	Common::LockGuard lock(m_mutex);

	if (m_closed) {
		return KERNEL_ERROR_EBADF;
	}
	auto it = std::find_if(m_events.begin(), m_events.end(),
	                       [ident = event.event.ident, filter = event.event.filter](const auto& e) {
		                       return e.event.ident == ident && e.event.filter == filter;
	                       });
	if (it != m_events.end()) {
		if (event.write_watch) {
			// A descriptor EV_ADD replaces ALL registration metadata and its
			// identity binding, not just udata (including after fd close/reuse).
			*it = event;
		} else {
			// Callback filters own queued payloads/lifetime state. Preserve those
			// on re-add; timers also preserve already accumulated occurrences.
			TriggerExpiredTimers(MonotonicTimeNs());
			it->deadline_ns = event.deadline_ns;
			it->interval_ns = event.interval_ns;
			it->event.udata = event.event.udata;
			for (auto& pending: it->pending_events) {
				pending.udata = event.event.udata;
			}
		}
	} else {
		m_events.push_back(event);
	}

	m_cond_var.SignalAll();
	return OK;
}

int KernelEqueuePrivate::TriggerEvent(uintptr_t ident, int16_t filter, void* trigger_data) {
	Common::LockGuard lock(m_mutex);

	if (m_closed) {
		return KERNEL_ERROR_EBADF;
	}
	auto it = std::find_if(m_events.begin(), m_events.end(), [ident, filter](const auto& e) {
		return e.event.ident == ident && e.event.filter == filter;
	});
	if (it != m_events.end()) {
		auto& event = *it;

		if (event.filter.trigger_func != nullptr) {
			event.filter.trigger_func(&event, trigger_data);
		} else {
			event.triggered = true;
		}

		m_cond_var.Signal();

		return OK;
	}

	return KERNEL_ERROR_ENOENT;
}

static void UserEventTriggerFunc(KernelEqueueEvent* event, void* trigger_data) {
	EXIT_IF(event == nullptr);
	event->triggered   = true;
	event->event.data  = reinterpret_cast<intptr_t>(trigger_data);
	event->event.udata = trigger_data;
}

static void AmprEventTriggerFunc(KernelEqueueEvent* event, void* trigger_data) {
	EXIT_IF(event == nullptr);
	auto triggered_event = event->event;
	triggered_event.data = static_cast<intptr_t>(reinterpret_cast<uintptr_t>(trigger_data));
	if (event->triggered) {
		event->pending_events.push_back(triggered_event);
	} else {
		event->event     = triggered_event;
		event->triggered = true;
	}
}

static void UserEventResetFunc(KernelEqueueEvent* event) {
	EXIT_IF(event == nullptr);
	if ((event->event.flags & EV_CLEAR) != 0) {
		event->triggered    = false;
		event->event.fflags = 0;
		event->event.data   = 0;
	}
}

int KernelEqueuePrivate::DeleteEvent(uintptr_t ident, int16_t filter) {
	Common::LockGuard lock(m_mutex);

	if (m_closed) {
		return KERNEL_ERROR_EBADF;
	}
	auto it = std::find_if(m_events.begin(), m_events.end(), [ident, filter](const auto& e) {
		return e.event.ident == ident && e.event.filter == filter;
	});
	if (it != m_events.end()) {
		auto& event = *it;

		if (event.filter.delete_event_func != nullptr) {
			auto owner = event.filter.owner;
			event.filter.delete_event_func(m_handle, &event);
		}

		m_events.erase(it);

		return OK;
	}

	return KERNEL_ERROR_ENOENT;
}

KernelEqueueRef KernelPinEqueue(KernelEqueue eq) {
	if (eq == KERNEL_EQUEUE_INVALID) {
		return {};
	}

	Common::LockGuard lock(g_equeues_mutex);
	const auto        entry = g_equeues.find(eq);
	return entry != g_equeues.end() ? entry->second : KernelEqueueRef {};
}

int __attribute__((force_align_arg_pointer)) KYTY_SYSV_ABI KernelCreateEqueue(KernelEqueue* eq, const char* name) {
	PRINT_NAME();

	if (eq == nullptr || name == nullptr) {
		return KERNEL_ERROR_EINVAL;
	}

	{
		Common::LockGuard lock(g_equeues_mutex);
		if (g_next_equeue > static_cast<uint64_t>(std::numeric_limits<KernelEqueue>::max())) {
			EXIT("event queue handle space exhausted\n");
		}
		*eq        = static_cast<KernelEqueue>(g_next_equeue++);
		auto owner = std::make_shared<KernelEqueuePrivate>(*eq);
		owner->SetName(std::string(name));
		g_equeues.emplace(*eq, std::move(owner));
	}

	LOGF("\tEqueue create: %s\n", name);

	return OK;
}

int __attribute__((force_align_arg_pointer)) KYTY_SYSV_ABI KernelAddEvent(KernelEqueue eq, const KernelEqueueEvent& event) {
	auto owner = KernelPinEqueue(eq);
	if (!owner) {
		return KERNEL_ERROR_EBADF;
	}

	return owner->AddEvent(event);
}

int __attribute__((force_align_arg_pointer)) KYTY_SYSV_ABI KernelTriggerEvent(KernelEqueue eq, uintptr_t ident, int16_t filter,
                                     void* trigger_data) {
	NoteHleCall(g_library, g_module, __func__);
	HLE_SLOW_GUARD("equeue");
	auto owner = KernelPinEqueue(eq);
	if (!owner) {
		return KERNEL_ERROR_EBADF;
	}

	return owner->TriggerEvent(ident, filter, trigger_data);
}

int __attribute__((force_align_arg_pointer)) KYTY_SYSV_ABI KernelDeleteEvent(KernelEqueue eq, uintptr_t ident, int16_t filter) {
	auto owner = KernelPinEqueue(eq);
	if (!owner) {
		return KERNEL_ERROR_EBADF;
	}

	return owner->DeleteEvent(ident, filter);
}

int __attribute__((force_align_arg_pointer)) KYTY_SYSV_ABI KernelDeleteEqueue(KernelEqueue eq) {
	PRINT_NAME();

	KernelEqueueRef owner;

	{
		Common::LockGuard lock(g_equeues_mutex);
		const auto        entry = g_equeues.find(eq);
		if (entry == g_equeues.end()) {
			return KERNEL_ERROR_EBADF;
		}
		owner = std::move(entry->second);
		g_equeues.erase(entry);
	}

	LOGF("\tEqueue delete: %s\n", owner->GetName().c_str());
	owner->Close();

	return OK;
}

int __attribute__((force_align_arg_pointer)) KYTY_SYSV_ABI KernelWaitEqueue(KernelEqueue eq, KernelEvent* ev, int num, int* out,
                                   const KernelUseconds* timo) {
	PRINT_NAME();
	HLE_SLOW_GUARD("equeue");

	auto owner = KernelPinEqueue(eq);
	if (!owner) {
		return KERNEL_ERROR_EBADF;
	}

	if (ev == nullptr) {
		return KERNEL_ERROR_EFAULT;
	}

	if (num < 1) {
		return KERNEL_ERROR_EINVAL;
	}

	EXIT_NOT_IMPLEMENTED(out == nullptr);

	if (timo == nullptr) {
		*out = owner->WaitForEvents(ev, num, 0);
	}

	if (timo != nullptr) {
		if (*timo == 0) {
			*out = owner->GetTriggeredEvents(ev, num);
		} else {
			*out = owner->WaitForEvents(ev, num, *timo);
		}
	}

	if (*out == KERNEL_ERROR_EBADF) {
		return KERNEL_ERROR_EBADF;
	}
	if (*out == 0) {
		return KERNEL_ERROR_ETIMEDOUT;
	}

	return OK;
}

int __attribute__((force_align_arg_pointer)) KYTY_SYSV_ABI KernelAddUserEvent(KernelEqueue eq, int id) {
	PRINT_NAME();

	LOGF("\t user event add: eq = 0x%016" PRIx64 ", id = %d\n", static_cast<uint64_t>(eq), id);

	KernelEqueueEvent event {};
	event.event.ident         = static_cast<uintptr_t>(id);
	event.event.filter        = KERNEL_EVFILT_USER;
	event.event.flags         = EV_ADD;
	event.event.fflags        = 0;
	event.event.data          = 0;
	event.event.udata         = nullptr;
	event.filter.trigger_func = UserEventTriggerFunc;
	event.filter.reset_func   = UserEventResetFunc;

	return KernelAddEvent(eq, event);
}

int __attribute__((force_align_arg_pointer)) KYTY_SYSV_ABI KernelAddUserEventEdge(KernelEqueue eq, int id) {
	PRINT_NAME();

	LOGF("\t user event edge add: eq = 0x%016" PRIx64 ", id = %d\n", static_cast<uint64_t>(eq), id);

	KernelEqueueEvent event {};
	event.event.ident         = static_cast<uintptr_t>(id);
	event.event.filter        = KERNEL_EVFILT_USER;
	event.event.flags         = EV_ADD | EV_CLEAR;
	event.event.fflags        = 0;
	event.event.data          = 0;
	event.event.udata         = nullptr;
	event.filter.trigger_func = UserEventTriggerFunc;
	event.filter.reset_func   = UserEventResetFunc;

	return KernelAddEvent(eq, event);
}

int __attribute__((force_align_arg_pointer)) KYTY_SYSV_ABI KernelTriggerUserEvent(KernelEqueue eq, int id, void* udata) {
	PRINT_NAME();

	LOGF("\t user event trigger: eq = 0x%016" PRIx64 ", id = %d, udata = 0x%016" PRIx64 "\n",
	     static_cast<uint64_t>(eq), id, reinterpret_cast<uint64_t>(udata));

	return KernelTriggerEvent(eq, static_cast<uintptr_t>(id), KERNEL_EVFILT_USER, udata);
}

int __attribute__((force_align_arg_pointer)) KYTY_SYSV_ABI KernelTriggerUserEventForAll(int id, void* udata) {
	int                          triggered = 0;
	std::vector<KernelEqueueRef> queues;

	{
		Common::LockGuard lock(g_equeues_mutex);
		queues.reserve(g_equeues.size());
		for (const auto& [handle, queue]: g_equeues) {
			queues.push_back(queue);
		}
	}
	for (const auto& eq: queues) {
		if (eq->TriggerEvent(static_cast<uintptr_t>(id), KERNEL_EVFILT_USER, udata) == OK) {
			triggered++;
		}
	}

	return (triggered > 0 ? OK : KERNEL_ERROR_ENOENT);
}

int __attribute__((force_align_arg_pointer)) KYTY_SYSV_ABI KernelDeleteUserEvent(KernelEqueue eq, int id) {
	PRINT_NAME();

	LOGF("\t user event delete: eq = 0x%016" PRIx64 ", id = %d\n", static_cast<uint64_t>(eq), id);

	return KernelDeleteEvent(eq, static_cast<uintptr_t>(id), KERNEL_EVFILT_USER);
}

// sceKernelAddReadEvent(eq, fd, size, udata): EVFILT_READ watch on a descriptor.
// AgcCompositor registers its GnmCompositor listener sockets this way.
int __attribute__((force_align_arg_pointer)) KYTY_SYSV_ABI KernelAddReadEvent(KernelEqueue eq, int fd, size_t size, void* udata) {
	PRINT_NAME();
	LOGF("\t read event add: eq = 0x%016" PRIx64 ", fd = %d, size = %zu\n",
	     static_cast<uint64_t>(eq), fd, size);

	KernelEqueueEvent event {};
	event.event.ident  = static_cast<uintptr_t>(fd);
	event.event.filter = KERNEL_EVFILT_READ;
	event.event.flags  = EV_ADD | EV_CLEAR;
	event.event.data   = static_cast<intptr_t>(size);
	event.event.udata  = udata;
	return KernelAddEvent(eq, event);
}

int __attribute__((force_align_arg_pointer)) KYTY_SYSV_ABI KernelDeleteReadEvent(KernelEqueue eq, int fd) {
	PRINT_NAME();
	return KernelDeleteEvent(eq, static_cast<uintptr_t>(fd), KERNEL_EVFILT_READ);
}

int __attribute__((force_align_arg_pointer)) KYTY_SYSV_ABI KernelAddWriteEvent(KernelEqueue eq, int fd, size_t size, void* udata) {
	PRINT_NAME();
	auto owner = KernelPinEqueue(eq);
	if (!owner) { return KERNEL_ERROR_EBADF; }
	if (GuestDescriptors::IsSocketRange(fd)) {
		// A live socket cannot supply native write-event capacity through the
		// current backend. Closed reserved slots must still report EBADF.
		return Libs::Network::Net::IsSocket(fd) ? KERNEL_ERROR_EOPNOTSUPP : KERNEL_ERROR_EBADF;
	}
	try {
		auto watch = std::make_shared<KernelWriteEventWatch>();
		const int result = FileSystem::PinWriteEventFile(fd, size, &watch->file);
		if (result != OK) { return result; }
		KernelEqueueEvent event {};
		event.event.ident = static_cast<uintptr_t>(static_cast<intptr_t>(fd));
		event.event.filter = KERNEL_EVFILT_WRITE;
		event.event.flags = EV_ADD; // Native wrapper does not request EV_CLEAR.
		event.event.udata = udata;
		event.write_watch = std::move(watch);
		return owner->AddEvent(event);
	} catch (const std::bad_alloc&) {
		return KERNEL_ERROR_ENOMEM;
	}
}

int __attribute__((force_align_arg_pointer)) KYTY_SYSV_ABI KernelDeleteWriteEvent(KernelEqueue eq, int fd) {
	PRINT_NAME();
	return KernelDeleteEvent(eq, static_cast<uintptr_t>(static_cast<intptr_t>(fd)), KERNEL_EVFILT_WRITE);
}

static int AddTimerEvent(KernelEqueue eq, int id, uint64_t delay_ns, bool periodic, void* udata) {
	const auto now_ns = MonotonicTimeNs();
	KernelEqueueEvent event {};
	event.deadline_ns  = delay_ns <= UINT64_MAX - now_ns ? now_ns + delay_ns : UINT64_MAX;
	event.interval_ns  = periodic ? delay_ns : 0;
	event.event.ident  = static_cast<uintptr_t>(id);
	event.event.filter = periodic ? KERNEL_EVFILT_TIMER : KERNEL_EVFILT_HRTIMER;
	event.event.flags  = EV_ADD | (periodic ? EV_CLEAR : EV_ONESHOT);
	event.event.udata  = udata;
	return KernelAddEvent(eq, event);
}

int __attribute__((force_align_arg_pointer)) KYTY_SYSV_ABI KernelAddTimerEvent(KernelEqueue eq, int id, KernelUseconds usec, void* udata) {
	return AddTimerEvent(eq, id, static_cast<uint64_t>(usec) * 1000, true, udata);
}

int __attribute__((force_align_arg_pointer)) KYTY_SYSV_ABI KernelAddHRTimerEvent(KernelEqueue eq, int id, const KernelTimespec* ts,
                                        void* udata) {
	if (ts == nullptr) {
		return KERNEL_ERROR_EFAULT;
	}
	if (ts->tv_sec < 0 || ts->tv_nsec < 0 || ts->tv_nsec >= 1000000000ll ||
	    static_cast<uint64_t>(ts->tv_sec) >
	        (UINT64_MAX - static_cast<uint64_t>(ts->tv_nsec)) / 1000000000ull) {
		return KERNEL_ERROR_EINVAL;
	}

	const auto delay_ns =
	    static_cast<uint64_t>(ts->tv_sec) * 1000000000ull + static_cast<uint64_t>(ts->tv_nsec);
	return AddTimerEvent(eq, id, delay_ns, false, udata);
}

int __attribute__((force_align_arg_pointer)) KYTY_SYSV_ABI KernelDeleteHRTimerEvent(KernelEqueue eq, int id) {
	return KernelDeleteEvent(eq, static_cast<uintptr_t>(id), KERNEL_EVFILT_HRTIMER);
}

int __attribute__((force_align_arg_pointer)) KYTY_SYSV_ABI KernelAddTimerEventVsh(KernelEqueue eq, int id, uint64_t timeout_us, void* udata,
                                      uint64_t /*flags*/) {
	// ps5-vsh: 57ZK+ODEXWY sceKernelAddTimerEvent(eq, id, timeout, udata, ...).
	// Observed args (eq, id, 200000, udata, 2, 0): the value field reads as
	// microseconds (200ms-class timers). Funnel into the HR timer machinery so
	// armed timers really deliver to kernel queue waiters.
	PRINT_NAME();
	LOGF("\t eq = 0x%016" PRIx64 " id = %d timeout_us = %" PRIu64 " udata = %p\n",
	     static_cast<uint64_t>(eq), id, timeout_us, udata);
	KernelTimespec ts {};
	ts.tv_sec  = static_cast<int64_t>(timeout_us / 1000000ull);
	ts.tv_nsec = static_cast<int64_t>((timeout_us % 1000000ull) * 1000ull);
	return KernelAddHRTimerEvent(eq, id, &ts, udata);
}

int __attribute__((force_align_arg_pointer)) KYTY_SYSV_ABI KernelAddAmprEvent(KernelEqueue eq, int id, void* udata) {
	PRINT_NAME();

	LOGF("\t AMPR event add: eq = 0x%016" PRIx64 ", id = %d, udata = 0x%016" PRIx64 "\n",
	     static_cast<uint64_t>(eq), id, reinterpret_cast<uint64_t>(udata));

	if (eq != KERNEL_EQUEUE_INVALID) {
		KernelEqueueEvent event {};
		event.event.ident         = static_cast<uintptr_t>(id);
		event.event.filter        = KERNEL_EVFILT_AMPR;
		event.event.flags         = EV_ADD | EV_CLEAR;
		event.event.fflags        = 0;
		event.event.data          = 0;
		event.event.udata         = udata;
		event.filter.trigger_func = AmprEventTriggerFunc;
		event.filter.reset_func   = UserEventResetFunc;
		(void)KernelAddEvent(eq, event);
	}

	return OK;
}

int __attribute__((force_align_arg_pointer)) KYTY_SYSV_ABI KernelAddAmprSystemEvent(KernelEqueue eq, int id, void* udata) {
	PRINT_NAME();

	LOGF("\t AMPR system event add: eq = 0x%016" PRIx64 ", id = %d, udata = 0x%016" PRIx64 "\n",
	     static_cast<uint64_t>(eq), id, reinterpret_cast<uint64_t>(udata));

	return KernelAddAmprEvent(eq, id, udata);
}

int __attribute__((force_align_arg_pointer)) KYTY_SYSV_ABI KernelDeleteAmprEvent(KernelEqueue eq, int id) {
	PRINT_NAME();

	LOGF("\t AMPR event delete: eq = 0x%016" PRIx64 ", id = %d\n", static_cast<uint64_t>(eq), id);

	if (eq != KERNEL_EQUEUE_INVALID) {
		(void)KernelDeleteEvent(eq, static_cast<uintptr_t>(id), KERNEL_EVFILT_AMPR);
	}

	return OK;
}

int __attribute__((force_align_arg_pointer)) KYTY_SYSV_ABI KernelDeleteAmprSystemEvent(KernelEqueue eq, int id) {
	PRINT_NAME();

	LOGF("\t AMPR system event delete: eq = 0x%016" PRIx64 ", id = %d\n", static_cast<uint64_t>(eq),
	     id);

	return KernelDeleteAmprEvent(eq, id);
}

intptr_t __attribute__((force_align_arg_pointer)) KYTY_SYSV_ABI KernelGetEventData(const KernelEvent* ev) {
	PRINT_NAME();

	if (ev != nullptr) {
		return ev->data;
	}

	return 0;
}

intptr_t __attribute__((force_align_arg_pointer)) KYTY_SYSV_ABI KernelGetEventFflags(const KernelEvent* ev) {
	PRINT_NAME();

	if (ev != nullptr) {
		return ev->fflags;
	}

	return 0;
}

int __attribute__((force_align_arg_pointer)) KYTY_SYSV_ABI KernelGetEventFilter(const KernelEvent* ev) {
	PRINT_NAME();

	if (ev != nullptr) {
		return ev->filter;
	}

	return 0;
}

uintptr_t __attribute__((force_align_arg_pointer)) KYTY_SYSV_ABI KernelGetEventId(const KernelEvent* ev) {
	PRINT_NAME();

	if (ev != nullptr) {
		return ev->ident;
	}

	return 0;
}

void* __attribute__((force_align_arg_pointer)) KYTY_SYSV_ABI KernelGetEventUserData(const KernelEvent* ev) {
	PRINT_NAME();

	if (ev != nullptr) {
		return ev->udata;
	}

	return nullptr;
}

int __attribute__((force_align_arg_pointer)) KYTY_SYSV_ABI KernelGetEventError(const KernelEvent* ev) {
	PRINT_NAME();

	if (ev != nullptr && (ev->flags & EV_ERROR) != 0) {
		return static_cast<int>(ev->data);
	}

	return 0;
}

} // namespace Libs::LibKernel::EventQueue

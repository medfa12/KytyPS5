#ifndef EMULATOR_INCLUDE_EMULATOR_KERNEL_GUESTFSCONTEXT_H_
#define EMULATOR_INCLUDE_EMULATOR_KERNEL_GUESTFSCONTEXT_H_

#include <cstdint>
#include <memory>
#include <mutex>
#include <string>

namespace Libs::LibKernel::FileSystem {

// A launch owns this object; its pthreads share it. Neither a module ID nor a
// synthetic PID identifies it. Paths are guest POSIX names, never host paths.
// Root stays /: this is not a chroot implementation or a vnode-backed cwd.
struct GuestFsSnapshot {
	std::string root = "/";
	std::string cwd = "/";
	uint64_t generation = 0;
	bool bound = false;
};

class GuestFsContext {
public:
	GuestFsContext() = default;
	explicit GuestFsContext(const GuestFsSnapshot& initial): m_root(initial.root), m_cwd(initial.cwd) {}
	GuestFsSnapshot Snapshot();
	// Validation happens outside the lock. A concurrent chdir forces a retry
	// against the new cwd instead of committing a stale relative lookup.
	bool TryChangeDirectory(uint64_t generation, const std::string& cwd);
private:
	std::mutex m_mutex;
	std::string m_root = "/";
	std::string m_cwd = "/";
	uint64_t m_generation = 0;
};

using GuestFsContextPtr = std::shared_ptr<GuestFsContext>;
GuestFsContextPtr CreateBootstrapFsContext();
GuestFsContextPtr CloneFsContext(const GuestFsContextPtr& parent);
GuestFsContextPtr GetCurrentFsContext();
void SetCurrentFsContext(GuestFsContextPtr context);

class ScopedGuestFsContext {
public:
	explicit ScopedGuestFsContext(GuestFsContextPtr context);
	~ScopedGuestFsContext();
	ScopedGuestFsContext(const ScopedGuestFsContext&) = delete;
	ScopedGuestFsContext& operator=(const ScopedGuestFsContext&) = delete;
private:
	GuestFsContextPtr m_previous;
};

} // namespace Libs::LibKernel::FileSystem
#endif

#pragma once

#include <array>
#include <cstdint>
#include <filesystem>
#include <span>
#include <string>
#include <system_error>

namespace Libs::LocalUserStorage {

// Host-only. Caller resolves these from the mounted guest paths; no guessed
// roots, guest pointers, JSON, credentials or implicit mounts live here.
struct Layout {
	// /user/home, /system_data/priv/home, /system_data/eap/rodata/home
	std::array<std::filesystem::path, 3> roots;
	std::filesystem::path journal;
};
enum class Lifetime { Persistent, Guest };
enum class Code { Ok, InvalidLayout, Collision, NotManaged, InvalidReceipt, IoError };
struct Result {
	Code code = Code::Ok;
	std::error_code error;
	std::filesystem::path path;
	// A failed rollback/cleanup or ambiguous durable write needs retry/recovery.
	// In particular, a failed Commit after record publication is NOT a failed save.
	bool recovery_required = false;
	explicit operator bool() const { return code == Code::Ok; }
};

class Transaction {
public:
	Transaction() = default;
	Transaction(const Transaction&) = delete;
	Transaction& operator=(const Transaction&) = delete;
	// Deliberately no automatic destructor rollback: an authoritative save may
	// already have committed. An abandoned transaction is reconciled by Recover.
	bool Pending() const { return active; }

private:
	friend struct Implementation;
	Layout layout;
	uint32_t user = 0;
	Lifetime lifetime = Lifetime::Persistent;
	std::string token;
	bool destroying = false;
	bool upgrading = false;
	bool active = false;
	bool committed = false;
};

// Roots/journal must already exist, be absolute, distinct/non-nested, and have
// no symlink/reparse ancestors. No other writer may mutate these paths while an
// operation runs. Caller serializes ALL calls with record publication and keeps
// guest storage users quiescent during prepare-destroy/rollback/retirement.
// IDs must not be reused while their receipt survives.
// Receipts/ownership files are flushed before namespace promotion. Windows
// promotions use MOVEFILE_WRITE_THROUGH; POSIX uses parent-directory fsync.
// Restart reconciliation covers process interruption, not an assertion of
// hardware/power-loss atomicity across the three independent filesystems.
//
// PrepareCreate provisions all roots and user-root webkit defaults before
// success. PrepareDestroy quarantines all owned roots (no physical deletion).
// On prepare failure rollback is attempted; Pending()/recovery_required report
// retained work. Unmanaged final directories are never adopted or removed.
Result PrepareCreate(const Layout& layout, uint32_t user, Lifetime lifetime, Transaction& transaction);
// Startup-only provisioning for an ALREADY committed persistent record. Requires
// all three final roots and its receipt absent; never adopts existing storage.
// Keep consumers quiescent until Commit succeeds: it durably converts upgrade
// intent to a normal persistent receipt. On Commit failure, do not admit users
// or Rollback (publication may have succeeded); retry Commit or restart recovery.
Result PrepareUpgrade(const Layout& layout, uint32_t user, Transaction& transaction);
Result PrepareDestroy(const Layout& layout, uint32_t user, Transaction& transaction);

// After successful authoritative record save/publication: Commit. After failed
// save: Rollback. A creation Commit retains durable ownership receipts for later
// deletion. A destruction Commit can return an error with cleanup retained;
// the record must stay deleted, and Commit or Recover can be retried.
Result Commit(Transaction& transaction);
Result Rollback(Transaction& transaction);

// Before admitting profile consumers, with no outstanding transactions. Input
// is the COMPLETE successfully loaded persistent record set, NOT an empty set
// substituted after a JSON read/parse error. Guests never survive a restart.
// Creates lacking a committed record are retired; interrupted deletions with a
// committed record are restored. Missing/corrupt receipts never authorize
// deleting an unmanaged directory. Stops at first error; retry is idempotent.
// Uncommitted upgrade intents retire only their owned storage, EVEN when the ID
// is in the committed set. After successful recovery the caller may retry
// PrepareUpgrade. Committed upgrades are ordinary live persistent receipts.
// Only receipt-managed storage is reconciled. Existing records/directories
// without receipts are not implicitly adopted or claimed to be provisioned.
Result Recover(const Layout& layout, std::span<const uint32_t> committed_persistent_users);

} // namespace Libs::LocalUserStorage

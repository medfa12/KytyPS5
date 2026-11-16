// Independent TU: the shared runner may declare/call TestLocalUserStorage().
// Define KYTY_LOCAL_USER_STORAGE_STANDALONE only for an isolated executable.
#include "libs/localUserStorage.h"

#include <array>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <sstream>
#include <stdexcept>
#include <string>
#include <vector>

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <Windows.h>
#include <sddl.h>
#endif

namespace {
namespace Storage = Libs::LocalUserStorage;
namespace fs = std::filesystem;
void Require(bool condition, const char* message) {
	if (!condition) { throw std::runtime_error(std::string("LocalUserStorage: ") + message); }
}
void Success(const Storage::Result& result, const char* message) {
	if (!result) {
		throw std::runtime_error(std::string("LocalUserStorage: ") + message + " code=" + std::to_string(static_cast<int>(result.code)) +
		    " path=" + result.path.string() + " error=" + result.error.message());
	}
}
void Write(const fs::path& path, const std::string& value) {
	std::ofstream stream(path, std::ios::binary);
	stream << value;
	stream.close();
	Require(!stream.fail(), "write fixture");
}
std::string Read(const fs::path& path) {
	std::ifstream stream(path, std::ios::binary);
	Require(stream.good(), "read fixture");
	return {std::istreambuf_iterator<char>(stream), std::istreambuf_iterator<char>()};
}
std::string User(uint32_t user) {
	char text[9];
	std::snprintf(text, sizeof(text), "%08x", user);
	return text;
}
struct Fixture {
	fs::path directory;
	Storage::Layout layout;
	Fixture() {
		const char* override_path = std::getenv("KYTY_LOCAL_STORAGE_TEST_ROOT");
		const auto base = override_path && *override_path ? fs::path(override_path) : fs::temp_directory_path();
		directory = base / ("kyty-user-storage-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
		Require(fs::create_directory(directory), "fresh isolated fixture");
		layout = {{directory / "user", directory / "private", directory / "eap"}, directory / "journal"};
		for (const auto& root: layout.roots) { Require(fs::create_directory(root), "explicit root"); }
		Require(fs::create_directory(layout.journal), "explicit journal");
	}
	~Fixture() { std::error_code error; fs::remove_all(directory, error); }
	void Present(uint32_t user) const {
		for (const auto& root: layout.roots) { Require(fs::is_directory(root / User(user)), "all profile roots visible"); }
	}
	void Absent(uint32_t user) const {
		for (const auto& root: layout.roots) { Require(!fs::exists(root / User(user)), "profile root absent"); }
	}
	void Empty() const {
		for (const auto& root: layout.roots) { Require(fs::is_empty(root), "no leaked staging/quarantine"); }
		Require(fs::is_empty(layout.journal), "no leaked receipt");
	}
	void Create(uint32_t user, Storage::Lifetime lifetime = Storage::Lifetime::Persistent) {
		Storage::Transaction transaction;
		Success(Storage::PrepareCreate(layout, user, lifetime, transaction), "create");
		Present(user);
		Success(Storage::Commit(transaction), "publish record");
		Require(!transaction.Pending(), "commit deactivates coordinator");
	}
};

void CreationAndSaveFailure() {
	Fixture fixture;
	constexpr uint32_t user = 0x10010000;
	Storage::Transaction transaction;
	Success(Storage::PrepareCreate(fixture.layout, user, Storage::Lifetime::Persistent, transaction), "prepare before record save");
	fixture.Present(user);
	for (const char* name: {"webbrowser", "webbrowser_dialog", "shell", "secure"}) {
		Require(fs::is_directory(fixture.layout.roots[0] / User(user) / "webkit" / name), "native webkit defaults provisioned");
	}
	Require(!fs::is_empty(fixture.layout.journal), "durable recovery evidence exists before publication");
	// Simulate the external authoritative record writer rejecting its save.
	Success(Storage::Rollback(transaction), "failed save rolls back all storage");
	fixture.Empty();
	Require(!transaction.Pending(), "rollback completes coordinator");
	fixture.Create(user);
	Success(Storage::Recover(fixture.layout, std::array {user}), "committed persistent survives restart");
	fixture.Present(user);
}

void CollisionAndLayout() {
	Fixture fixture;
	constexpr uint32_t user = 0x10010001;
	const auto unmanaged = fixture.layout.roots[1] / User(user);
	fs::create_directory(unmanaged);
	Write(unmanaged / "keep", "unmanaged data");
	Storage::Transaction transaction;
	const auto collision = Storage::PrepareCreate(fixture.layout, user, Storage::Lifetime::Persistent, transaction);
	Require(collision.code == Storage::Code::Collision && !transaction.Pending(), "unmanaged collision rejects create");
	Require(Read(unmanaged / "keep") == "unmanaged data", "collision never adopts/removes content");
	Require(fs::is_empty(fixture.layout.roots[0]) && fs::is_empty(fixture.layout.roots[2]), "collision publishes no partial root");
	Require(!Storage::PrepareDestroy(fixture.layout, user, transaction), "unmanaged delete refused");
	Success(Storage::Recover(fixture.layout, {}), "unmanaged directory ignored by recovery");
	Require(Read(unmanaged / "keep") == "unmanaged data", "recovery preserves unrelated profile");
	auto nested = fixture.layout;
	nested.journal = nested.roots[0];
	Require(Storage::PrepareCreate(nested, user + 1, Storage::Lifetime::Persistent, transaction).code == Storage::Code::InvalidLayout,
	    "overlapping roots rejected");
	auto relative = fixture.layout;
	relative.roots[0] = "relative-user-home";
	Require(Storage::PrepareCreate(relative, user + 1, Storage::Lifetime::Persistent, transaction).code == Storage::Code::InvalidLayout,
	    "relative guesses rejected");
}

void DeleteSaveFailureAndRecovery() {
	Fixture fixture;
	constexpr uint32_t user = 0x10010002;
	fixture.Create(user);
	for (const auto& root: fixture.layout.roots) { Write(root / User(user) / "payload", "preserve through failed delete"); }
	{
		Storage::Transaction transaction;
		Success(Storage::PrepareDestroy(fixture.layout, user, transaction), "quarantine before record deletion");
		fixture.Absent(user);
		Success(Storage::Rollback(transaction), "failed authoritative delete restores all roots");
	}
	for (const auto& root: fixture.layout.roots) { Require(Read(root / User(user) / "payload") == "preserve through failed delete", "rollback preserved bytes"); }
	{
		Storage::Transaction abandoned;
		Success(Storage::PrepareDestroy(fixture.layout, user, abandoned), "crash after quarantine");
	}
	Success(Storage::Recover(fixture.layout, std::array {user}), "old committed record restores quarantined storage");
	fixture.Present(user);
	{
		Storage::Transaction abandoned;
		Success(Storage::PrepareDestroy(fixture.layout, user, abandoned), "quarantine again");
		// Record deletion succeeds, then process exits before cleanup.
	}
	Success(Storage::Recover(fixture.layout, {}), "deleted record retires quarantine after crash");
	fixture.Empty();
	Success(Storage::Recover(fixture.layout, {}), "recovery idempotent");
}

void CreateCrashAndGuestRecovery() {
	Fixture fixture;
	constexpr uint32_t orphan = 0x10010003, persistent = 0x10010004, guest = 0x20010000;
	{
		Storage::Transaction abandoned;
		Success(Storage::PrepareCreate(fixture.layout, orphan, Storage::Lifetime::Persistent, abandoned), "promote before simulated crash");
	}
	fixture.Create(persistent);
	fixture.Create(guest, Storage::Lifetime::Guest);
	Success(Storage::Recover(fixture.layout, std::array {persistent}), "recover mixed committed, uncommitted and guest receipts");
	fixture.Absent(orphan);
	fixture.Absent(guest);
	fixture.Present(persistent);
	fixture.Create(guest, Storage::Lifetime::Guest);
	Storage::Transaction logout;
	Success(Storage::PrepareDestroy(fixture.layout, guest, logout), "guest logout quarantines storage");
	Success(Storage::Commit(logout), "guest logout retires storage");
	fixture.Absent(guest);
	fixture.Present(persistent);
}

void ReceiptAndRollbackConflicts() {
	Fixture fixture;
	constexpr uint32_t user = 0x10010006;
	fixture.Create(user);
	const auto journal = fs::directory_iterator(fixture.layout.journal)->path();
	const auto valid_receipt = Read(journal);
	Write(journal, "truncated receipt");
	Require(!Storage::Recover(fixture.layout, {}), "corrupt receipt does not authorize removal");
	fixture.Present(user);
	Write(journal, valid_receipt);
	Storage::Transaction transaction;
	Success(Storage::PrepareDestroy(fixture.layout, user, transaction), "quarantine conflict fixture");
	const auto conflicting = fixture.layout.roots[1] / User(user);
	fs::create_directory(conflicting);
	Write(conflicting / "unrelated", "must not overwrite");
	const auto result = Storage::Rollback(transaction);
	Require(!result && result.recovery_required && transaction.Pending(), "rollback collision retained for explicit recovery");
	Require(Read(conflicting / "unrelated") == "must not overwrite", "rollback does not overwrite unrelated directory");
	Require(!Storage::Recover(fixture.layout, std::array {user}), "recovery also refuses collision");
	Require(Read(conflicting / "unrelated") == "must not overwrite", "recovery preserves conflicting bytes");
	// Only the test fixture owner removes the conflicting foreign directory.
	fs::remove_all(conflicting);
	Success(Storage::Rollback(transaction), "resume partial restoration after external conflict removed");
	fixture.Present(user);
	Success(Storage::Recover(fixture.layout, std::array {user}), "restoration left a consistent live receipt");
}

void UpgradeCrashRecovery() {
	constexpr uint32_t user = 0x10010009;
	// Reconstruct interruption boundaries from genuinely provisioned storage and
	// its generated ownership evidence, without production fault-injection hooks.
	// 0: intent only; 1: partial staging; 2: before promotion; 3: partial promotion;
	// 4: all promoted, but Commit has not published the upgraded storage.
	for (int point = 0; point < 5; ++point) {
		Fixture fixture;
		{
			Storage::Transaction abandoned;
			Success(Storage::PrepareUpgrade(fixture.layout, user, abandoned), "prepare existing record upgrade");
		}
		std::istringstream marker(Read(fixture.layout.roots[0] / User(user) / ".kyty-local-user-owner"));
		std::string magic, id, token;
		Require(static_cast<bool>(marker >> magic >> id >> token), "read actual upgrade ownership evidence");
		for (size_t i = 0; i < fixture.layout.roots.size(); ++i) {
			const auto& root = fixture.layout.roots[i];
			const auto final = root / User(user);
			if (point == 0 || (point == 1 && i != 0)) {
				fs::remove_all(final); // This root had not yet been provisioned.
			} else if (point <= 2 || (point == 3 && i != 0)) {
				fs::rename(final, root / (".kyty-user-" + id + "-" + token + ".stage"));
			}
			Write(root / "unrelated", "never owned by upgrade");
		}
		// Unlike ordinary creation, the authoritative record predates every point.
		Success(Storage::Recover(fixture.layout, std::array {user}), "partial upgrade recoverable with pre-existing authoritative ID");
		fixture.Absent(user);
		Require(fs::is_empty(fixture.layout.journal), "unpublished upgrade intent retired");
		for (const auto& root: fixture.layout.roots) {
			Require(Read(root / "unrelated") == "never owned by upgrade", "upgrade cleanup preserves unrelated bytes");
			Require(std::distance(fs::directory_iterator(root), fs::directory_iterator {}) == 1, "partial upgrade leaves no staging or quarantine");
		}
		Storage::Transaction retry;
		Success(Storage::PrepareUpgrade(fixture.layout, user, retry), "caller retries upgrade after recovery");
		Success(Storage::Commit(retry), "durably publish upgrade before consumers");
		Require(!retry.Pending() && !Storage::Rollback(retry), "published upgrade cannot roll back");
		for (const auto& root: fixture.layout.roots) { Write(root / User(user) / "payload", "consumer data after publication"); }
		for (int restart = 0; restart < 2; ++restart) {
			Success(Storage::Recover(fixture.layout, std::array {user}), "committed upgrade survives restart");
			for (const auto& root: fixture.layout.roots) {
				Require(Read(root / User(user) / "payload") == "consumer data after publication", "published upgrade data never retired");
			}
		}
	}
}

void UpgradeCollisionAndRollback() {
	constexpr uint32_t user = 0x1001000a;
	for (size_t collision = 0; collision < 3; ++collision) {
		Fixture fixture;
		const auto foreign = fixture.layout.roots[collision] / User(user);
		fs::create_directory(foreign);
		Write(foreign / "payload", "pre-existing home");
		Storage::Transaction transaction;
		const auto result = Storage::PrepareUpgrade(fixture.layout, user, transaction);
		Require(result.code == Storage::Code::Collision && !transaction.Pending(), "upgrade requires every home absent");
		Require(Read(foreign / "payload") == "pre-existing home" && fs::is_empty(fixture.layout.journal), "collision neither adopts data nor writes intent");
		for (size_t i = 0; i < 3; ++i) {
			if (i != collision) { Require(fs::is_empty(fixture.layout.roots[i]), "collision does not partially upgrade"); }
		}
	}
	Fixture fixture;
	Storage::Transaction transaction;
	Success(Storage::PrepareUpgrade(fixture.layout, user, transaction), "prepare upgrade rollback");
	Success(Storage::Rollback(transaction), "upgrade rollback before publication");
	fixture.Empty();
	Success(Storage::Recover(fixture.layout, std::array {user}), "record-only user remains retryable after rollback");
	Success(Storage::PrepareUpgrade(fixture.layout, user, transaction), "retry rolled back upgrade");
	Success(Storage::Commit(transaction), "publish retried upgrade");
	const auto journal = fixture.layout.journal / ("user-" + User(user) + ".receipt");
	const auto receipt = Read(journal);
	Storage::Transaction duplicate;
	Require(Storage::PrepareUpgrade(fixture.layout, user, duplicate).code == Storage::Code::Collision, "existing managed user cannot be upgraded again");
	Require(Read(journal) == receipt, "duplicate upgrade preserves live receipt");
}

void InterruptedPendingRecovery() {
	constexpr uint32_t user = 0x10010007;
	const std::array<std::string, 2> fragments {std::string {}, std::string("KYTY-LOCAL-USER-1\n\0partial", 26)};
	for (const auto& fragment: fragments) {
		Fixture fixture;
		const auto pending = fixture.layout.journal / ("user-" + User(user) + ".receipt.pending");
		const auto first_archive = fixture.layout.journal / ("archived-" + pending.filename().string() + "-0");
		const auto archive = fixture.layout.journal / ("archived-" + pending.filename().string() + "-1");
		Write(first_archive, "earlier interrupted write");
		Write(pending, fragment); // Crash before the first receipt promotion.
		Success(Storage::Recover(fixture.layout, {}), "unpromoted partial write does not block startup");
		Require(!fs::exists(pending), "reserved pending name released");
		Require(Read(first_archive) == "earlier interrupted write", "archive collision never overwrites evidence");
		Require(Read(archive) == fragment, "empty/binary partial write archived byte-for-byte");
		for (const auto& root: fixture.layout.roots) { Require(fs::is_empty(root), "pending bytes authorize no provisioning"); }
		fixture.Create(user); // Same ID can now retry its exclusive receipt write.
		Success(Storage::Recover(fixture.layout, std::array {user}), "archived partial write ignored on subsequent startup");
		fixture.Present(user);
		Require(Read(archive) == fragment, "retry preserves archived artifact");
	}
	// Live receipt, deletion interrupted before record save, and deletion after
	// record save. The same malformed pending bytes must not change any decision.
	for (int state = 0; state < 3; ++state) {
		Fixture fixture;
		fixture.Create(user);
		for (const auto& root: fixture.layout.roots) { Write(root / User(user) / "payload", "committed data"); }
		const auto journal = fixture.layout.journal / ("user-" + User(user) + ".receipt");
		const auto live_receipt = Read(journal);
		if (state != 0) {
			Storage::Transaction abandoned;
			Success(Storage::PrepareDestroy(fixture.layout, user, abandoned), "interrupt deletion before cleanup");
			fixture.Absent(user);
		}
		const auto pending = fs::path(journal.string() + ".pending");
		const auto archive = fixture.layout.journal / ("archived-" + pending.filename().string() + "-0");
		Write(pending, fragments[1]);
		const std::vector<uint32_t> committed = state == 2 ? std::vector<uint32_t> {} : std::vector<uint32_t> {user};
		Success(Storage::Recover(fixture.layout, committed), "promoted receipt alone determines recovery");
		Require(!fs::exists(pending) && Read(archive) == fragments[1], "partial update archived intact");
		if (state == 2) {
			for (const auto& root: fixture.layout.roots) { Require(fs::is_empty(root), "committed deletion retires all storage"); }
			Require(!fs::exists(journal), "completed retirement removes promoted receipt");
		} else {
			fixture.Present(user);
			for (const auto& root: fixture.layout.roots) {
				Require(Read(root / User(user) / "payload") == "committed data", "live record preserves every root's data");
			}
			Require(Read(journal) == live_receipt, "live ownership receipt preserved or restored");
		}
		Success(Storage::Recover(fixture.layout, committed), "recovery after archival is idempotent");
		Require(Read(archive) == fragments[1], "repeated recovery preserves archived bytes");
	}
}

void PendingNeverAuthorizesRecovery() {
	Fixture fixture;
	constexpr uint32_t user = 0x10010008;
	fixture.Create(user);
	const auto journal = fixture.layout.journal / ("user-" + User(user) + ".receipt");
	const auto pending = fs::path(journal.string() + ".pending");
	const auto receipt = Read(journal);
	// Even a fully valid unpromoted receipt cannot authorize deletion or adoption.
	fs::rename(journal, pending);
	for (const auto& root: fixture.layout.roots) { Write(root / User(user) / "payload", "unmanaged now"); }
	Success(Storage::Recover(fixture.layout, {}), "valid pending receipt is still non-authoritative");
	for (const auto& root: fixture.layout.roots) {
		Require(Read(root / User(user) / "payload") == "unmanaged now", "pending receipt cannot retire existing directories");
	}
	Require(Read(fixture.layout.journal / ("archived-" + pending.filename().string() + "-0")) == receipt,
	    "fully written pending receipt archived intact");
	// Unsafe source entries remain untouched rather than being followed.
	const auto outside = fixture.directory / "outside";
	Write(outside, "external bytes");
	std::error_code error;
	fs::create_symlink(outside, pending, error);
	if (error) {
		std::printf("LocalUserStorage: symlink pending test skipped: %s\n", error.message().c_str());
		return;
	}
	const auto result = Storage::Recover(fixture.layout, {});
	Require(!result && result.recovery_required, "pending symlink refused");
	Require(fs::is_symlink(fs::symlink_status(pending)) && Read(outside) == "external bytes", "symlink and target untouched");
	fs::remove(pending);
	// A dangling archive destination must also count as occupied.
	const auto occupied = fixture.layout.journal / ("archived-" + pending.filename().string() + "-1");
	fs::create_symlink(fixture.directory / "absent", occupied);
	Write(pending, "another partial write");
	Success(Storage::Recover(fixture.layout, {}), "archive skips dangling destination link");
	Require(fs::is_symlink(fs::symlink_status(occupied)), "archive never replaces dangling link");
	Require(Read(fixture.layout.journal / ("archived-" + pending.filename().string() + "-2")) == "another partial write",
	    "archive uses next unoccupied name");
}

#ifdef _WIN32
struct NoDeleteHandle {
	HANDLE handle;
	explicit NoDeleteHandle(const fs::path& path, bool directory): handle(CreateFileW(path.c_str(), GENERIC_READ,
	    FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING, directory ? FILE_FLAG_BACKUP_SEMANTICS : FILE_ATTRIBUTE_NORMAL, nullptr)) {
		Require(handle != INVALID_HANDLE_VALUE, "open actual no-delete-sharing handle");
	}
	~NoDeleteHandle() { CloseHandle(handle); }
};
// Actual filesystem denial, not a production fault-injection hook. Resolve the
// security APIs dynamically so the integrated target needs no new link library.
struct DenySubdirectory {
	using Get = decltype(&GetFileSecurityW);
	using Set = decltype(&SetFileSecurityW);
	using Convert = decltype(&ConvertStringSecurityDescriptorToSecurityDescriptorW);
	HMODULE library = LoadLibraryW(L"advapi32.dll");
	fs::path path;
	std::vector<unsigned char> original;
	Set set = nullptr;
	bool changed = false;
	explicit DenySubdirectory(const fs::path& target): path(target) {
		Require(library != nullptr, "security API available");
		const auto get = reinterpret_cast<Get>(GetProcAddress(library, "GetFileSecurityW"));
		set = reinterpret_cast<Set>(GetProcAddress(library, "SetFileSecurityW"));
		const auto convert = reinterpret_cast<Convert>(GetProcAddress(library, "ConvertStringSecurityDescriptorToSecurityDescriptorW"));
		Require(get && set && convert, "security API entrypoints");
		DWORD size = 0;
		get(path.c_str(), DACL_SECURITY_INFORMATION, nullptr, 0, &size);
		Require(size != 0, "read original DACL size");
		original.resize(size);
		Require(get(path.c_str(), DACL_SECURITY_INFORMATION, original.data(), size, &size) != 0, "retain original DACL");
		PSECURITY_DESCRIPTOR descriptor = nullptr;
		Require(convert(L"D:P(D;;0x00000004;;;WD)(A;;FA;;;WD)", SDDL_REVISION_1, &descriptor, nullptr) != 0, "deny directory creation descriptor");
		changed = set(path.c_str(), DACL_SECURITY_INFORMATION | PROTECTED_DACL_SECURITY_INFORMATION, descriptor) != 0;
		LocalFree(descriptor);
		Require(changed, "install deterministic directory creation denial");
	}
	~DenySubdirectory() {
		if (changed) { set(path.c_str(), DACL_SECURITY_INFORMATION, original.data()); }
		if (library) { FreeLibrary(library); }
	}
};

void UpgradeCommitFailure() {
	Fixture fixture;
	constexpr uint32_t user = 0x1001000b;
	const auto journal = fixture.layout.journal / ("user-" + User(user) + ".receipt");
	const auto pending = fs::path(journal.string() + ".pending");
	std::string unpromoted;
	{
		Storage::Transaction abandoned;
		Success(Storage::PrepareUpgrade(fixture.layout, user, abandoned), "prepare upgrade commit failure");
		const auto intent = Read(journal);
		NoDeleteHandle hold(journal, false);
		const auto result = Storage::Commit(abandoned);
		Require(!result && result.recovery_required && abandoned.Pending(), "failed upgrade publication requires blocked admission");
		Require(Read(journal) == intent, "failed replacement leaves upgrade intent authoritative");
		unpromoted = Read(pending);
		Require(unpromoted != intent, "normal live receipt written but not promoted");
		Require(!Storage::Rollback(abandoned), "failed commit cannot risk undoing ambiguous publication");
	}
	Success(Storage::Recover(fixture.layout, std::array {user}), "failed upgrade commit recovered despite pre-existing record");
	fixture.Absent(user);
	Require(!fs::exists(journal) && !fs::exists(pending), "uncommitted upgrade retired and pending isolated");
	const auto archive = fixture.layout.journal / ("archived-" + pending.filename().string() + "-0");
	Require(Read(archive) == unpromoted, "unpromoted live receipt preserved but never grants publication");
	Storage::Transaction retry;
	Success(Storage::PrepareUpgrade(fixture.layout, user, retry), "retry failed upgrade commit after recovery");
	Success(Storage::Commit(retry), "publish recovered upgrade");
	for (const auto& root: fixture.layout.roots) { Write(root / User(user) / "payload", "published data"); }
	// An interrupted unrelated pending write cannot demote a published upgrade.
	Write(pending, "truncated update");
	Success(Storage::Recover(fixture.layout, std::array {user}), "normal receipt wins over pending after upgrade publication");
	for (const auto& root: fixture.layout.roots) { Require(Read(root / User(user) / "payload") == "published data", "committed upgrade bytes preserved"); }
}

void RealFilesystemFailures() {
	Fixture fixture;
	constexpr uint32_t user = 0x10010005;
	{
		DenySubdirectory deny(fixture.layout.roots[2]);
		Storage::Transaction transaction;
		const auto result = Storage::PrepareCreate(fixture.layout, user, Storage::Lifetime::Persistent, transaction);
		Require(!result && !result.recovery_required && !transaction.Pending(), "third-root provisioning failure rolls back first two roots");
		fixture.Empty();
	}
	fixture.Create(user);
	Write(fixture.layout.roots[0] / User(user) / "payload", "open file contents");
	{
		NoDeleteHandle hold(fixture.layout.roots[1] / User(user), true);
		Storage::Transaction transaction;
		const auto result = Storage::PrepareDestroy(fixture.layout, user, transaction);
		if (result || result.recovery_required || transaction.Pending()) {
			throw std::runtime_error("LocalUserStorage: second-root rename failure rollback code=" + std::to_string(static_cast<int>(result.code)) +
			    " retained=" + std::to_string(result.recovery_required) + " pending=" + std::to_string(transaction.Pending()) +
			    " path=" + result.path.string() + " error=" + result.error.message());
		}
		fixture.Present(user);
	}
	Require(Read(fixture.layout.roots[0] / User(user) / "payload") == "open file contents", "failed delete preserved data");
	Storage::Transaction transaction;
	Success(Storage::PrepareDestroy(fixture.layout, user, transaction), "prepare cleanup failure");
	const auto retired = fs::directory_iterator(fixture.layout.roots[0])->path();
	{
		NoDeleteHandle hold(retired / "payload", false);
		const auto result = Storage::Commit(transaction); // Authoritative record is now absent.
		Require(!result && result.recovery_required && transaction.Pending(), "blocked physical cleanup reported and retained");
		Require(Read(retired / "payload") == "open file contents", "blocked data remains accessible");
		Require(!fs::is_empty(fixture.layout.journal), "cleanup retains durable authority");
		Require(!Storage::Rollback(transaction), "cannot undo authoritative commit after destructive cleanup starts");
	}
	Success(Storage::Commit(transaction), "cleanup retry after handle closes");
	fixture.Empty();
	const auto pending = fixture.layout.journal / ("user-" + User(user) + ".receipt.pending");
	const auto archive = fixture.layout.journal / ("archived-" + pending.filename().string() + "-0");
	Write(pending, "interrupted write held open");
	{
		NoDeleteHandle hold(pending, false);
		const auto result = Storage::Recover(fixture.layout, {});
		Require(!result && result.recovery_required, "blocked pending archival reports retained recovery work");
		Require(Read(pending) == "interrupted write held open" && !fs::exists(archive), "failed archival preserves original bytes");
	}
	Success(Storage::Recover(fixture.layout, {}), "pending archival retries after handle closes");
	Require(!fs::exists(pending) && Read(archive) == "interrupted write held open", "archive retry preserves all bytes");
	fixture.Create(user);
}
#endif
} // namespace

void TestLocalUserStorage() {
	CreationAndSaveFailure();
	CollisionAndLayout();
	DeleteSaveFailureAndRecovery();
	CreateCrashAndGuestRecovery();
	ReceiptAndRollbackConflicts();
	UpgradeCrashRecovery();
	UpgradeCollisionAndRollback();
	InterruptedPendingRecovery();
	PendingNeverAuthorizesRecovery();
#ifdef _WIN32
	UpgradeCommitFailure();
	RealFilesystemFailures();
#endif
}

#ifdef KYTY_LOCAL_USER_STORAGE_STANDALONE
int main() {
	try { TestLocalUserStorage(); std::puts("LocalUserStorage: passed"); return 0; }
	catch (const std::exception& error) { std::fprintf(stderr, "%s\n", error.what()); return 1; }
}
#endif

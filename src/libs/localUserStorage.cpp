#include "libs/localUserStorage.h"

#include <algorithm>
#include <cerrno>
#include <cstdio>
#include <fstream>
#include <iomanip>
#include <random>
#include <sstream>
#include <string>
#include <vector>

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <Windows.h>
#else
#include <fcntl.h>
#include <unistd.h>
#ifdef __linux__
#include <linux/fs.h>
#include <sys/syscall.h>
#endif
#endif

namespace Libs::LocalUserStorage {
namespace {
namespace fs = std::filesystem;
constexpr const char* OwnerFile = ".kyty-local-user-owner";
struct Receipt {
	uint32_t user = 0;
	Lifetime lifetime = Lifetime::Persistent;
	bool destroying = false;
	bool upgrading = false;
	std::string token;
	Layout layout;
};
Result Failure(Code code, const fs::path& path, std::error_code error = {}, bool retained = false) {
	return {code, error, path, retained};
}
std::error_code SystemError() {
#ifdef _WIN32
	return {static_cast<int>(GetLastError()), std::system_category()};
#else
	return {errno, std::generic_category()};
#endif
}
std::string Utf8(const fs::path& path) {
	const auto text = path.generic_u8string();
	return {reinterpret_cast<const char*>(text.data()), text.size()};
}
std::string Id(uint32_t user) {
	char text[9];
	std::snprintf(text, sizeof(text), "%08x", user);
	return text;
}
bool Hex(const std::string& text, size_t size) {
	return text.size() == size && text.find_first_not_of("0123456789abcdef") == std::string::npos;
}
fs::path Journal(const Receipt& receipt) { return receipt.layout.journal / ("user-" + Id(receipt.user) + ".receipt"); }
fs::path Final(const Receipt& receipt, size_t root) { return receipt.layout.roots[root] / Id(receipt.user); }
fs::path Private(const Receipt& receipt, size_t root, bool trash) {
	return receipt.layout.roots[root] / (".kyty-user-" + Id(receipt.user) + "-" + receipt.token + (trash ? ".retired" : ".stage"));
}
bool Exists(const fs::path& path, std::error_code& error) {
	const auto status = fs::symlink_status(path, error);
	if (error == std::errc::no_such_file_or_directory) { error.clear(); return false; }
	return !error && status.type() != fs::file_type::not_found;
}
bool PlainDirectory(const fs::path& path, std::error_code& error) {
	const auto status = fs::symlink_status(path, error);
	if (error || !fs::is_directory(status) || fs::is_symlink(status)) { return false; }
#ifdef _WIN32
	const auto attributes = GetFileAttributesW(path.c_str());
	if (attributes == INVALID_FILE_ATTRIBUTES) { error = SystemError(); return false; }
	if (attributes & FILE_ATTRIBUTE_REPARSE_POINT) { return false; }
#endif
	return true;
}
// Receipts bind to normalized absolute roots. Validation deliberately walks
// ancestors BEFORE canonicalization, so a junction cannot silently change scope.
Result Validate(const Layout& input, Layout& output) {
	std::array<fs::path, 4> paths {input.roots[0], input.roots[1], input.roots[2], input.journal};
	for (auto& path: paths) {
		if (!path.is_absolute()) { return Failure(Code::InvalidLayout, path); }
		path = path.lexically_normal();
		for (auto part = path; !part.empty();) {
			std::error_code error;
			if (!PlainDirectory(part, error)) { return Failure(Code::InvalidLayout, part, error); }
			const auto parent = part.parent_path();
			if (parent == part) { break; }
			part = parent;
		}
		std::error_code error;
		path = fs::canonical(path, error);
		if (error) { return Failure(Code::InvalidLayout, path, error); }
	}
	for (size_t i = 0; i < paths.size(); ++i) {
		for (size_t j = i + 1; j < paths.size(); ++j) {
			// equivalent() also detects aliases not caught by string normalization.
			for (auto ancestor = paths[i]; !ancestor.empty();) {
				std::error_code error;
				if (fs::equivalent(ancestor, paths[j], error)) { return Failure(Code::InvalidLayout, paths[j]); }
				if (error) { return Failure(Code::InvalidLayout, ancestor, error); }
				const auto parent = ancestor.parent_path();
				if (parent == ancestor) { break; }
				ancestor = parent;
			}
			for (auto ancestor = paths[j]; !ancestor.empty();) {
				std::error_code error;
				if (fs::equivalent(ancestor, paths[i], error)) { return Failure(Code::InvalidLayout, paths[i]); }
				if (error) { return Failure(Code::InvalidLayout, ancestor, error); }
				const auto parent = ancestor.parent_path();
				if (parent == ancestor) { break; }
				ancestor = parent;
			}
		}
	}
	output = {{paths[0], paths[1], paths[2]}, paths[3]};
	return {};
}
std::string Encode(const Receipt& receipt) {
	std::ostringstream text;
	text << "KYTY-LOCAL-USER-1\n" << Id(receipt.user) << ' ' << receipt.token << ' '
	     << (receipt.lifetime == Lifetime::Guest ? 'G' : 'P') << ' '
	     << (receipt.destroying ? 'D' : receipt.upgrading ? 'U' : 'C') << '\n';
	for (const auto& root: receipt.layout.roots) { text << std::quoted(Utf8(root)) << '\n'; }
	text << std::quoted(Utf8(receipt.layout.journal)) << '\n';
	return text.str();
}
Result ReadText(const fs::path& path, std::string& text) {
	std::error_code error;
	const auto status = fs::symlink_status(path, error);
	if (error || !fs::is_regular_file(status) || fs::is_symlink(status)) { return Failure(Code::InvalidReceipt, path, error, true); }
#ifdef _WIN32
	const auto attributes = GetFileAttributesW(path.c_str());
	if (attributes == INVALID_FILE_ATTRIBUTES || (attributes & FILE_ATTRIBUTE_REPARSE_POINT)) { return Failure(Code::InvalidReceipt, path, {}, true); }
#endif
	const auto size = fs::file_size(path, error);
	if (error || size > 65536) { return Failure(Code::InvalidReceipt, path, error, true); }
	std::ifstream stream(path, std::ios::binary);
	text.resize(static_cast<size_t>(size));
	if (!stream.read(text.data(), static_cast<std::streamsize>(size))) { return Failure(Code::IoError, path, {}, true); }
	return {};
}
Result ReadReceipt(const fs::path& path, const Layout& layout, Receipt& receipt) {
	std::string text;
	if (auto result = ReadText(path, text); !result) { return result; }
	std::istringstream stream(text);
	std::string magic, id, life, operation;
	stream >> magic >> id >> receipt.token >> life >> operation;
	if (magic != "KYTY-LOCAL-USER-1" || !Hex(id, 8) || !Hex(receipt.token, 32) ||
	    (life != "P" && life != "G") || (operation != "C" && operation != "D" && operation != "U") ||
	    (operation == "U" && life != "P")) {
		return Failure(Code::InvalidReceipt, path, {}, true);
	}
	receipt.user = static_cast<uint32_t>(std::stoul(id, nullptr, 16));
	receipt.lifetime = life == "G" ? Lifetime::Guest : Lifetime::Persistent;
	receipt.destroying = operation == "D";
	receipt.upgrading = operation == "U";
	receipt.layout = layout;
	for (const auto& root: layout.roots) {
		std::string saved;
		if (!(stream >> std::quoted(saved)) || saved != Utf8(root)) { return Failure(Code::InvalidReceipt, path, {}, true); }
	}
	std::string saved, extra;
	if (!(stream >> std::quoted(saved)) || saved != Utf8(layout.journal) || (stream >> extra)) {
		return Failure(Code::InvalidReceipt, path, {}, true);
	}
	return {};
}
Result SyncDirectory(const fs::path& path) {
#ifdef _WIN32
	// Windows has no supported FlushFileBuffers contract for a directory handle.
	// File markers are flushed; namespace promotions use MOVEFILE_WRITE_THROUGH.
	(void)path;
	return {};
#else
	const int fd = open(path.c_str(), O_RDONLY | O_DIRECTORY);
	if (fd < 0) { return Failure(Code::IoError, path, SystemError(), true); }
	const int result = fsync(fd);
	const auto error = result ? SystemError() : std::error_code {};
	close(fd);
	return result ? Failure(Code::IoError, path, error, true) : Result {};
#endif
}
Result WriteExclusive(const fs::path& path, const std::string& text) {
#ifdef _WIN32
	HANDLE file = CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_NEW, FILE_ATTRIBUTE_NORMAL, nullptr);
	if (file == INVALID_HANDLE_VALUE) { return Failure(Code::IoError, path, SystemError(), true); }
	DWORD written = 0;
	const bool ok = WriteFile(file, text.data(), static_cast<DWORD>(text.size()), &written, nullptr) &&
	    written == text.size() && FlushFileBuffers(file);
	const auto error = ok ? std::error_code {} : SystemError();
	const bool closed = CloseHandle(file) != 0;
	if (!ok || !closed) { return Failure(Code::IoError, path, ok ? SystemError() : error, true); }
#else
	const int fd = open(path.c_str(), O_WRONLY | O_CREAT | O_EXCL, 0600);
	if (fd < 0) { return Failure(Code::IoError, path, SystemError(), true); }
	size_t offset = 0;
	while (offset < text.size()) {
		const auto written = write(fd, text.data() + offset, text.size() - offset);
		if (written < 0 && errno == EINTR) { continue; }
		if (written <= 0) { const auto error = SystemError(); close(fd); return Failure(Code::IoError, path, error, true); }
		offset += static_cast<size_t>(written);
	}
	const int synced = fsync(fd);
	const auto error = synced ? SystemError() : std::error_code {};
	const int closed = close(fd);
	if (synced || closed) { return Failure(Code::IoError, path, synced ? error : SystemError(), true); }
#endif
	return SyncDirectory(path.parent_path());
}
Result Move(const fs::path& source, const fs::path& target, bool replace = false) {
#ifdef _WIN32
	if (!MoveFileExW(source.c_str(), target.c_str(), MOVEFILE_WRITE_THROUGH | (replace ? MOVEFILE_REPLACE_EXISTING : 0))) {
		return Failure(Code::IoError, target, SystemError(), true);
	}
#else
	int result = -1;
	if (replace) { result = rename(source.c_str(), target.c_str()); }
#ifdef __linux__
	else { result = static_cast<int>(syscall(SYS_renameat2, AT_FDCWD, source.c_str(), AT_FDCWD, target.c_str(), RENAME_NOREPLACE)); }
#else
	else { errno = ENOTSUP; }
#endif
	if (result) { return Failure(Code::IoError, target, SystemError(), true); }
#endif
	if (auto result = SyncDirectory(source.parent_path()); !result) { return result; }
	return source.parent_path() == target.parent_path() ? Result {} : SyncDirectory(target.parent_path());
}
// Unpromoted bytes carry no authority, even when they happen to parse. Keep
// them for inspection outside the reserved receipt namespace, without reading
// through links or replacing an earlier archive. Caller excludes other writers.
Result ArchivePending(const fs::path& path) {
	std::error_code error;
	const auto status = fs::symlink_status(path, error);
	if (error || !fs::is_regular_file(status) || fs::is_symlink(status)) {
		return Failure(Code::InvalidReceipt, path, error, true);
	}
#ifdef _WIN32
	const auto attributes = GetFileAttributesW(path.c_str());
	if (attributes == INVALID_FILE_ATTRIBUTES) { return Failure(Code::IoError, path, SystemError(), true); }
	if (attributes & FILE_ATTRIBUTE_REPARSE_POINT) { return Failure(Code::InvalidReceipt, path, {}, true); }
#endif
	for (uint64_t index = 0; index != UINT64_MAX; ++index) {
		const auto archive = path.parent_path() / ("archived-" + path.filename().string() + "-" + std::to_string(index));
		const bool exists = Exists(archive, error); // Includes dangling links; never follow archive candidates.
		if (error) { return Failure(Code::IoError, archive, error, true); }
		if (!exists) { return Move(path, archive); } // Atomic no-replacement rename, not copy/delete.
	}
	return Failure(Code::Collision, path, {}, true);
}
Result RemoveFile(const fs::path& path) {
	std::error_code error;
	fs::remove(path, error);
	if (error) { return Failure(Code::IoError, path, error, true); }
	return SyncDirectory(path.parent_path());
}
Result SaveReceipt(const Receipt& receipt, bool replace) {
	const auto path = Journal(receipt);
	auto pending = path;
	pending += ".pending";
	if (auto result = WriteExclusive(pending, Encode(receipt)); !result) { return result; }
	return Move(pending, path, replace);
}
std::string Owner(const Receipt& receipt) { return "KYTY-LOCAL-USER-1 " + Id(receipt.user) + " " + receipt.token + "\n"; }
Result Owned(const fs::path& path, const Receipt& receipt) {
	std::error_code error;
	if (!PlainDirectory(path, error)) { return Failure(Code::Collision, path, error, true); }
	std::string marker;
	if (auto result = ReadText(path / OwnerFile, marker); !result) { return result; }
	return marker == Owner(receipt) ? Result {} : Failure(Code::Collision, path, {}, true);
}
// Keep the ownership marker until ALL other contents are removed. A blocked
// file must not erase the only evidence needed to retry cleanup after restart.
Result CleanPrivate(const fs::path& path, const Receipt& receipt) {
	std::error_code error;
	if (!Exists(path, error)) { return error ? Failure(Code::IoError, path, error, true) : Result {}; }
	if (!PlainDirectory(path, error)) { return Failure(Code::Collision, path, error, true); }
	if (fs::is_empty(path, error) && !error) { return RemoveFile(path); }
	if (error) { return Failure(Code::IoError, path, error, true); }
	if (auto result = Owned(path, receipt); !result) { return result; }
	fs::directory_iterator iterator(path, error), end;
	while (!error && iterator != end) {
		const auto child = iterator->path();
		iterator.increment(error);
		if (error) { break; }
		if (child.filename() == OwnerFile) { continue; }
		fs::remove_all(child, error); // Does not follow symlinks out of this owned tree.
	}
	if (error) { return Failure(Code::IoError, path, error, true); }
	if (auto result = RemoveFile(path / OwnerFile); !result) { return result; }
	return RemoveFile(path);
}
Result NoEntry(const fs::path& path) {
	std::error_code error;
	const bool exists = Exists(path, error);
	if (error) { return Failure(Code::IoError, path, error); }
	return exists ? Failure(Code::Collision, path) : Result {};
}
Result CurrentReceipt(const Receipt& receipt) {
	Receipt current;
	if (auto result = ReadReceipt(Journal(receipt), receipt.layout, current); !result) { return result; }
	return current.user == receipt.user && current.token == receipt.token && current.lifetime == receipt.lifetime ?
	    Result {} : Failure(Code::Collision, Journal(receipt), {}, true);
}
Result Retire(const Receipt& receipt) {
	if (auto result = CurrentReceipt(receipt); !result) { return result; }
	for (size_t i = 0; i < 3; ++i) {
		const auto final = Final(receipt, i), trash = Private(receipt, i, true);
		std::error_code error;
		if (Exists(final, error)) {
			if (auto result = Owned(final, receipt); !result) { return result; }
			if (auto result = NoEntry(trash); !result) { result.recovery_required = true; return result; }
			if (auto result = Move(final, trash); !result) { return result; }
		} else if (error) { return Failure(Code::IoError, final, error, true); }
		if (auto result = CleanPrivate(trash, receipt); !result) { return result; }
		if (auto result = CleanPrivate(Private(receipt, i, false), receipt); !result) { return result; }
	}
	return RemoveFile(Journal(receipt));
}
Result Restore(const Receipt& receipt) {
	if (auto result = CurrentReceipt(receipt); !result) { return result; }
	for (size_t i = 0; i < 3; ++i) {
		const auto final = Final(receipt, i), trash = Private(receipt, i, true);
		std::error_code error;
		if (Exists(trash, error)) {
			if (auto result = Owned(trash, receipt); !result) { return result; }
			if (auto result = NoEntry(final); !result) { result.recovery_required = true; return result; }
			if (auto result = Move(trash, final); !result) { return result; }
		} else if (error) { return Failure(Code::IoError, trash, error, true); }
		if (auto result = Owned(final, receipt); !result) { return result; }
	}
	Receipt restored = receipt;
	restored.destroying = false;
	return SaveReceipt(restored, true);
}
std::string NewToken() {
	std::random_device random;
	std::ostringstream text;
	for (int i = 0; i < 4; ++i) { text << std::hex << std::setw(8) << std::setfill('0') << static_cast<uint32_t>(random()); }
	return text.str();
}
Result ExceptionFailure() { return Failure(Code::IoError, {}, std::make_error_code(std::errc::io_error), true); }
} // namespace

struct Implementation {
	static Receipt Get(const Transaction& transaction) {
		return {transaction.user, transaction.lifetime, transaction.destroying, transaction.upgrading, transaction.token, transaction.layout};
	}
	static void Set(Transaction& transaction, const Receipt& receipt) {
		transaction.layout = receipt.layout;
		transaction.user = receipt.user;
		transaction.lifetime = receipt.lifetime;
		transaction.destroying = receipt.destroying;
		transaction.upgrading = receipt.upgrading;
		transaction.token = receipt.token;
		transaction.active = true;
		transaction.committed = false;
	}
	static Result Rollback(Transaction& transaction) {
		if (!transaction.active || transaction.committed) { return Failure(Code::NotManaged, {}, {}, transaction.active); }
		const auto receipt = Get(transaction);
		const auto result = receipt.destroying ? Restore(receipt) : Retire(receipt);
		if (result) { transaction.active = false; }
		return result;
	}
	static Result FailedPrepare(Transaction& transaction, Result failure) {
		if (const auto rollback = Rollback(transaction); !rollback) { return rollback; }
		failure.recovery_required = false;
		return failure;
	}
	static Result Create(const Layout& layout, uint32_t user, Lifetime lifetime, Transaction& transaction, bool upgrading = false) {
		if (transaction.active) { return Failure(Code::Collision, {}, {}, true); }
		Receipt receipt {user, lifetime, false, upgrading, NewToken(), {}};
		if (auto result = Validate(layout, receipt.layout); !result) { return result; }
		if (auto result = NoEntry(Journal(receipt)); !result) { return result; }
		for (size_t i = 0; i < 3; ++i) {
			for (const auto& path: {Final(receipt, i), Private(receipt, i, false), Private(receipt, i, true)}) {
				if (auto result = NoEntry(path); !result) { return result; }
			}
		}
		// No path is provisioned until its durable intent/ownership receipt exists.
		if (auto result = SaveReceipt(receipt, false); !result) { return result; }
		Set(transaction, receipt);
		for (size_t i = 0; i < 3; ++i) {
			const auto stage = Private(receipt, i, false);
			std::error_code error;
			if (!fs::create_directory(stage, error)) { return FailedPrepare(transaction, Failure(Code::IoError, stage, error)); }
			if (auto result = WriteExclusive(stage / OwnerFile, Owner(receipt)); !result) { return FailedPrepare(transaction, result); }
			if (i == 0) {
				for (const auto* directory: {"webbrowser", "webbrowser_dialog", "shell", "secure"}) {
					fs::create_directories(stage / "webkit" / directory, error);
					if (error) { return FailedPrepare(transaction, Failure(Code::IoError, stage, error)); }
				}
				if (auto result = SyncDirectory(stage / "webkit"); !result) { return FailedPrepare(transaction, result); }
			}
			if (auto result = SyncDirectory(stage); !result) { return FailedPrepare(transaction, result); }
		}
		for (size_t i = 0; i < 3; ++i) {
			if (auto result = Move(Private(receipt, i, false), Final(receipt, i)); !result) { return FailedPrepare(transaction, result); }
		}
		return {};
	}
	static Result Destroy(const Layout& layout, uint32_t user, Transaction& transaction) {
		if (transaction.active) { return Failure(Code::Collision, {}, {}, true); }
		Receipt receipt;
		if (auto result = Validate(layout, receipt.layout); !result) { return result; }
		receipt.user = user;
		const auto journal = Journal(receipt);
		std::error_code error;
		if (!Exists(journal, error)) {
			return Failure(error ? Code::IoError : Code::NotManaged, journal, error);
		}
		if (auto result = ReadReceipt(journal, receipt.layout, receipt); !result) { return result; }
		if (receipt.user != user || receipt.destroying || receipt.upgrading) { return Failure(Code::InvalidReceipt, journal, {}, true); }
		for (size_t i = 0; i < 3; ++i) {
			if (auto result = Owned(Final(receipt, i), receipt); !result) { return result; }
			for (bool trash: {false, true}) { if (auto result = NoEntry(Private(receipt, i, trash)); !result) { return result; } }
		}
		receipt.destroying = true;
		if (auto result = SaveReceipt(receipt, true); !result) { return result; }
		Set(transaction, receipt);
		for (size_t i = 0; i < 3; ++i) {
			if (auto result = Move(Final(receipt, i), Private(receipt, i, true)); !result) { return FailedPrepare(transaction, result); }
		}
		return {};
	}
	static Result Commit(Transaction& transaction) {
		if (!transaction.active) { return Failure(Code::NotManaged, {}); }
		transaction.committed = true;
		const auto receipt = Get(transaction);
		if (auto result = CurrentReceipt(receipt); !result) { return result; }
		if (receipt.destroying) {
			if (auto result = Retire(receipt); !result) { return result; }
		} else {
			for (size_t i = 0; i < 3; ++i) { if (auto result = Owned(Final(receipt, i), receipt); !result) { return result; } }
			if (receipt.upgrading) {
				Receipt published = receipt;
				published.upgrading = false;
				// Until this durable transition succeeds, a live authoritative ID
				// must not cause recovery to accept a partially provisioned upgrade.
				if (auto result = SaveReceipt(published, true); !result) { return result; }
				transaction.upgrading = false;
			}
		}
		transaction.active = false;
		return {};
	}
};

Result PrepareCreate(const Layout& layout, uint32_t user, Lifetime lifetime, Transaction& transaction) try {
	return Implementation::Create(layout, user, lifetime, transaction);
} catch (...) { return ExceptionFailure(); }
Result PrepareUpgrade(const Layout& layout, uint32_t user, Transaction& transaction) try {
	return Implementation::Create(layout, user, Lifetime::Persistent, transaction, true);
} catch (...) { return ExceptionFailure(); }
Result PrepareDestroy(const Layout& layout, uint32_t user, Transaction& transaction) try {
	return Implementation::Destroy(layout, user, transaction);
} catch (...) { return ExceptionFailure(); }
Result Commit(Transaction& transaction) try { return Implementation::Commit(transaction); } catch (...) { return ExceptionFailure(); }
Result Rollback(Transaction& transaction) try { return Implementation::Rollback(transaction); } catch (...) { return ExceptionFailure(); }

Result Recover(const Layout& input, std::span<const uint32_t> committed) try {
	Layout layout;
	if (auto result = Validate(input, layout); !result) { return result; }
	std::error_code error;
	std::vector<fs::path> journals, pending;
	for (fs::directory_iterator iterator(layout.journal, error), end; !error && iterator != end; iterator.increment(error)) {
		const auto name = iterator->path().filename().string();
		if (name.size() < 13 || name.substr(0, 5) != "user-" || !Hex(name.substr(5, 8), 8)) { continue; }
		if (name.substr(13) == ".receipt") { journals.push_back(iterator->path()); }
		if (name.substr(13) == ".receipt.pending") { pending.push_back(iterator->path()); }
	}
	if (error) { return Failure(Code::IoError, layout.journal, error, true); }
	// An interrupted exclusive write may be empty/truncated. Archive all pending
	// bytes without interpreting them; only promoted receipts drive reconciliation.
	for (const auto& path: pending) {
		if (auto result = ArchivePending(path); !result) { return result; }
	}
	std::sort(journals.begin(), journals.end());
	for (const auto& path: journals) {
		Receipt receipt;
		if (auto result = ReadReceipt(path, layout, receipt); !result) { return result; }
		if (Journal(receipt) != path) { return Failure(Code::InvalidReceipt, path, {}, true); }
		const bool live = receipt.lifetime == Lifetime::Persistent && std::find(committed.begin(), committed.end(), receipt.user) != committed.end();
		if (receipt.upgrading || !live) { if (auto result = Retire(receipt); !result) { return result; } }
		else if (receipt.destroying) { if (auto result = Restore(receipt); !result) { return result; } }
		else {
			for (size_t i = 0; i < 3; ++i) { if (auto result = Owned(Final(receipt, i), receipt); !result) { return result; } }
		}
	}
	return {};
} catch (...) { return ExceptionFailure(); }

} // namespace Libs::LocalUserStorage

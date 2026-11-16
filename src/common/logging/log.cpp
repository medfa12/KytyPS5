
#include "common/logging/log.h"

#include "common/assert.h"
#include "common/emulatorConfig.h"
#include "common/threads.h"

#include <cstdio>
#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <fmt/format.h>
#include <memory>
#include <mutex>
#include <thread>
#include <spdlog/formatter.h>
#include <spdlog/logger.h>
#include <spdlog/sinks/basic_file_sink.h>
#include <spdlog/sinks/null_sink.h>
#include <spdlog/sinks/stdout_sinks.h>
#include <string_view>

namespace {

class RawFormatter final: public spdlog::formatter {
public:
	void format(const spdlog::details::log_msg& msg, spdlog::memory_buf_t& dest) override {
		const std::string_view payload(msg.payload.data(), msg.payload.size());
		dest.append(payload.data(), payload.data() + payload.size());
	}

	std::unique_ptr<spdlog::formatter> clone() const override {
		return std::make_unique<RawFormatter>();
	}
};

std::shared_ptr<spdlog::logger> MakeLogger(std::string name, spdlog::sink_ptr sink) {
	sink->set_formatter(std::make_unique<RawFormatter>());

	auto logger = std::make_shared<spdlog::logger>(std::move(name), std::move(sink));
	logger->set_level(spdlog::level::trace);
	return logger;
}

std::shared_ptr<spdlog::logger> MakeFileLogger(std::string                  name,
                                               const std::filesystem::path& path) {
	const auto parent = path.parent_path();
	if (!parent.empty()) {
		std::filesystem::create_directories(parent);
	}

	const bool truncate = std::getenv(Log::RelaunchedVariable) == nullptr;
	auto       sink = std::make_shared<spdlog::sinks::basic_file_sink_mt>(path.native(), truncate);
	return MakeLogger(std::move(name), std::move(sink));
}

static bool HasStyle(fmt::text_style style) {
	return style != fmt::text_style {};
}

void WriteStdout(std::string_view text, fmt::text_style style = {}) {
	Common::LoggingCriticalGuard critical;
	if (HasStyle(style)) {
		fmt::print(stdout, style, "{}", text);
	} else {
		std::fwrite(text.data(), 1, text.size(), stdout);
	}
	std::fflush(stdout);
}

} // namespace

namespace Log {

static bool                            g_initialized = false;
static Direction                       g_direction   = Direction::Console;
static std::filesystem::path           g_output_file;
static std::mutex                      g_logger_mutex;
static std::shared_ptr<spdlog::logger> g_logger;

void Flush() {
	Common::LoggingCriticalGuard critical;
	if (g_logger != nullptr) {
		g_logger->flush();
	}
}

static void SetupLogger() {
	Common::LoggingCriticalGuard critical;
	std::lock_guard lock(g_logger_mutex);
	Flush();
	g_logger.reset();

	switch (g_direction) {
		case Direction::Silent:
			g_logger = MakeLogger("kyty", std::make_shared<spdlog::sinks::null_sink_mt>());
			break;
		case Direction::Console:
			g_logger = MakeLogger("kyty", std::make_shared<spdlog::sinks::stdout_sink_mt>());
			break;
		case Direction::File:
			if (!g_output_file.empty()) {
				g_logger = MakeFileLogger("kyty", g_output_file);
			}
			break;
	}
}

static std::shared_ptr<spdlog::logger> GetLogger() {
	std::lock_guard lock(g_logger_mutex);
	return g_logger;
}

static void WriteImpl(std::string_view text, fmt::text_style style = {}) {
	Common::LoggingCriticalGuard critical;
	if (text.empty()) {
		return;
	}

	if (!g_initialized) {
		WriteStdout(text, style);
		return;
	}

	if (g_direction == Direction::Silent) {
		return;
	}

	if (auto logger = GetLogger()) {
		if (g_direction == Direction::Console && HasStyle(style)) {
			const auto styled = fmt::format(style, "{}", text);
			logger->log(spdlog::level::info, spdlog::string_view_t(styled.data(), styled.size()));
		} else {
			logger->log(spdlog::level::info, spdlog::string_view_t(text.data(), text.size()));
		}
	}
}

void WriteToConsoleAndLog(std::string_view text) {
	Common::LoggingCriticalGuard critical;
	WriteImpl(text);
	if (g_initialized && g_direction != Direction::Console) {
		WriteStdout(text);
	}
	Flush();
}

void WriteFatal(std::string_view text) {
	if (g_direction == Direction::Silent || !g_initialized) {
		WriteStdout(text);
	} else {
		WriteStdout(text);
		WriteImpl(text);
	}
	Flush();
}

void WriteFatal(fmt::text_style style, std::string_view text) {
	if (g_direction == Direction::Silent || !g_initialized) {
		WriteStdout(text, style);
	} else {
		WriteStdout(text, style);
		WriteImpl(text, style);
	}
	Flush();
}

void Initialize() {
	g_initialized = true;
	switch (Config::GetPrintfDirection()) {
		case Config::LogDirection::Silent: g_direction = Direction::Silent; break;
		case Config::LogDirection::Console: g_direction = Direction::Console; break;
		case Config::LogDirection::File: g_direction = Direction::File; break;
	}
	g_output_file =
	    (g_direction == Direction::File ? Config::GetPrintfOutputFile() : std::filesystem::path {});
	SetupLogger();
	// ps5-vsh: periodic file flush so long boot logs are pollable and the
	// tail survives a hard crash (spdlog file sinks buffer otherwise).
	if (g_direction == Direction::File) {
		std::thread([] {
			for (;;) {
				std::this_thread::sleep_for(std::chrono::seconds(2));
				Flush();
			}
		}).detach();
	}
}

void Shutdown() {
	Common::LoggingCriticalGuard critical;
	Flush();
	std::lock_guard lock(g_logger_mutex);
	g_logger.reset();
}

Direction GetDirection() {
	EXIT_IF(!g_initialized);
	return g_direction;
}

bool IsSilent() {
	// Before init LOGF must keep writing to stdout, so report non-silent.
	return g_initialized && g_direction == Direction::Silent;
}

void Write(std::string_view text) {
	WriteImpl(text);
	// ps5-vsh: KYTY_FLUSH_LOG=1 flushes every line (diagnostic runs only;
	// makes the on-disk tail exact so the last line before a hard crash
	// names the crashing call).
	static const bool flush_each = std::getenv("KYTY_FLUSH_LOG") != nullptr;
	if (flush_each) {
		Flush();
	}
}

void Write(fmt::text_style style, std::string_view text) {
	WriteImpl(text, style);
}

#if defined(KYTY_VIRTUAL_MEMORY_ALLOCATION_TESTS)
// Exercise the real WriteToConsoleAndLog -> Flush -> base_sink mutex path.
void TestLoggingFlushWithCallback(void (*callback)()) {
	class Sink final: public spdlog::sinks::base_sink<std::mutex> {
	public:
		explicit Sink(void (*fn)()): m_callback(fn) {}
	private:
		void sink_it_(const spdlog::details::log_msg&) override {}
		void flush_() override { m_callback(); }
		void (*m_callback)();
	};
	auto saved = GetLogger();
	{
		Common::LoggingCriticalGuard critical;
		std::lock_guard lock(g_logger_mutex);
		g_logger = MakeLogger("logging-suspend-test", std::make_shared<Sink>(callback));
	}
	WriteToConsoleAndLog("");
	{
		Common::LoggingCriticalGuard critical;
		std::lock_guard lock(g_logger_mutex);
		g_logger = std::move(saved);
	}
}
#endif

} // namespace Log

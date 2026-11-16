// Include inside the memory-test fixture namespace; parent includes loader/elf.h,
// loader/runtimeLinker.h, common/file.h, <array>, <algorithm>, <chrono>, <filesystem>, <vector>.
// These fixtures use only synthetic temporary files, never firmware inputs.
namespace AuthorityMetadataTest {
constexpr const char* Name = "AuthorityMetadata";
constexpr std::array<uint8_t, 64> Header {
    0x7f, 0x45, 0x4c, 0x46, 2, 1, 1, 9, 2, 0, 0, 0, 0, 0, 0, 0,
    0x10, 0xfe, 0x3e, 0, 1, 0, 0, 0, 0x80, 0, 0, 0, 0, 0, 0, 0,
    0x40, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
    0, 0, 0, 0, 0x40, 0, 0x38, 0, 1, 0, 0, 0, 0, 0, 0, 0,
};
constexpr std::array<uint8_t, 56> Segment {
    1, 0, 0, 0, 5, 0, 0, 0, 0, 2, 0, 0, 0, 0, 0, 0,
    0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
    0x10, 0, 0, 0, 0, 0, 0, 0, 0x10, 0, 0, 0, 0, 0, 0, 0,
    0, 0x40, 0, 0, 0, 0, 0, 0,
};
// Literal local ShellCore record; fixture ELF layout deliberately differs.
constexpr std::array<uint8_t, 64> Record {
    0x10, 0, 0, 0, 0, 0, 0, 0x48, 8, 0, 0, 0, 0, 0, 0, 0,
    0, 0, 0, 0, 0, 0, 0x60, 0x13, 0, 0, 0, 0, 0, 0, 0x60, 0x13,
    0x41, 0x44, 0x07, 0x23, 0x59, 0x36, 0x10, 0x86,
    0x72, 0xd7, 0x14, 0xfc, 0xba, 0xf7, 0xee, 0xc1,
    0x39, 0x64, 0x7a, 0x47, 0xe7, 0x69, 0xde, 0x4c,
    0xc1, 0x27, 0x9a, 0xb9, 0x02, 0x06, 0xdf, 0x9d,
};

std::array<uint8_t, 0x210> Plain() {
	std::array<uint8_t, 0x210> bytes {};
	std::copy(Header.begin(), Header.end(), bytes.begin());
	std::copy(Segment.begin(), Segment.end(), bytes.begin() + 0x40);
	std::copy(Record.begin(), Record.end(), bytes.begin() + 0x78);
	return bytes;
}

std::array<uint8_t, 0x310> Self() {
	std::array<uint8_t, 0x310> bytes {};
	constexpr std::array<uint8_t, 64> wrapper {
	    0x54, 0x14, 0xf5, 0xee, 0x10, 1, 1, 0x12, 1, 1, 0, 0x10,
	    0xf8, 0, 8, 0, 0x10, 3, 0, 0, 0, 0, 0, 0, 1, 0, 0x32, 0, 0, 0, 0, 0,
	    0, 8, 0, 0, 0, 0, 0, 0, 0, 3, 0, 0, 0, 0, 0, 0,
	    0x10, 0, 0, 0, 0, 0, 0, 0, 0x10, 0, 0, 0, 0, 0, 0, 0,
	};
	std::copy(wrapper.begin(), wrapper.end(), bytes.begin());
	std::copy(Header.begin(), Header.end(), bytes.begin() + 0x40);
	std::copy(Segment.begin(), Segment.end(), bytes.begin() + 0x80);
	std::copy(Record.begin(), Record.end(), bytes.begin() + 0xb8);
	return bytes;
}
} // namespace AuthorityMetadataTest

void TestAuthorityMetadata() {
	using namespace AuthorityMetadataTest;
	using Provenance = Loader::AuthorityMetadataProvenance;
	const auto plain = Plain();
	const auto found = Loader::ReadAuthorityMetadata(plain, plain.size());
	Check(Name, found.has_value(), "literal extended-info fixture not recognized");
	Check(Name, found->authority_id == UINT64_C(0x4800000000000010) && found->program_type == 8 &&
	            found->firmware_version == UINT64_C(0x1360000000000000) &&
	            found->application_version == UINT64_C(0x1360000000000000) && found->file_offset == 0x78 &&
	            found->raw_record == Record && found->provenance == Provenance::RetainedExtendedInfo &&
	            std::equal(found->digest.begin(), found->digest.end(), Record.begin() + 32),
	      "record values, raw bytes, offset or provenance changed");
	auto ui = plain;
	ui[0x78] = 0x0f;
	const auto ui_metadata = Loader::ReadAuthorityMetadata(ui, ui.size());
	Check(Name, ui_metadata && ui_metadata->authority_id == UINT64_C(0x480000000000000f), "ShellUI ID lost");
	auto other_id = plain;
	std::fill_n(other_id.begin() + 0x78, 8, 0);
	other_id[0x78] = 0x42;
	const auto other = Loader::ReadAuthorityMetadata(other_id, other_id.size());
	Check(Name, other && other->authority_id == 0x42, "recognition must not whitelist privileged IDs");
	for (size_t length: {size_t {0}, size_t {31}, size_t {63}, size_t {64}, size_t {0x77}, size_t {0x78}, size_t {0xb7}}) {
		Check(Name, !Loader::ReadAuthorityMetadata(std::span<const uint8_t>(plain).first(length), plain.size()),
		      "truncated actual header bytes accepted");
	}
	Check(Name, !Loader::ReadAuthorityMetadata(plain, 0xb7), "declared file length cannot be exceeded");
	// Mutate each structural discriminator independently, retaining a privileged ID.
	for (size_t offset: {size_t {0}, size_t {4}, size_t {5}, size_t {7}, size_t {8}, size_t {9},
	                    size_t {16}, size_t {18}, size_t {20}, size_t {32}, size_t {40}, size_t {48},
	                    size_t {52}, size_t {54}, size_t {58}, size_t {60}, size_t {62},
	                    size_t {0x80}, size_t {0x88}, size_t {0x90}}) {
		auto bad = plain;
		bad[offset] ^= 0x80;
		Check(Name, !Loader::ReadAuthorityMetadata(bad, bad.size()), "malformed/unknown header or record accepted");
	}
	{
		auto bad = plain;
		std::fill(bad.begin() + 0x78, bad.begin() + 0xb8, 0);
		Check(Name, !Loader::ReadAuthorityMetadata(bad, bad.size()), "zero header padding acquired authority");
		bad = plain;
		std::fill_n(bad.begin() + 32, 8, 0xff);
		Check(Name, !Loader::ReadAuthorityMetadata(bad, UINT64_MAX), "overflowing program table offset accepted");
		bad = plain;
		bad[56] = bad[57] = 0xff;
		Check(Name, !Loader::ReadAuthorityMetadata(bad, UINT64_MAX), "unbounded/truncated program table accepted");
		bad = plain;
		bad[56] = 0;
		Check(Name, !Loader::ReadAuthorityMetadata(bad, bad.size()), "empty program table accepted");
		bad = plain;
		std::fill_n(bad.begin() + 0x48, 8, 0xff);
		Check(Name, !Loader::ReadAuthorityMetadata(bad, UINT64_MAX), "segment offset+length overflow accepted");
		bad = plain;
		bad[0x48] = 0x78;
		bad[0x49] = 0;
		Check(Name, !Loader::ReadAuthorityMetadata(bad, bad.size()), "record overlapping load payload accepted");
		bad[0x40] = 4;
		Check(Name, !Loader::ReadAuthorityMetadata(bad, bad.size()), "record overlapping non-load payload accepted");
		bad = plain;
		bad[0x49] = 3;
		Check(Name, !Loader::ReadAuthorityMetadata(bad, bad.size()), "missing load payload accepted");
	}
	{
		// Keep a valid load while testing a second, non-load segment. The selected
		// plain images similarly declare a trailing NOTE whose payload is absent.
		auto two = plain;
		two[56] = 2;
		std::copy(Segment.begin(), Segment.end(), two.begin() + 0x78);
		two[0x78] = 4;
		two[0x80] = 0x10; // NOTE [0x210, 0x220), just beyond EOF.
		std::copy(Record.begin(), Record.end(), two.begin() + 0xb0);
		const auto metadata = Loader::ReadAuthorityMetadata(two, two.size());
		Check(Name, metadata && metadata->file_offset == 0xb0 && metadata->raw_record == Record,
		      "metadata recognition incorrectly requires unrelated NOTE payload");
		two[0x80] = 0xb0;
		two[0x81] = 0;
		Check(Name, !Loader::ReadAuthorityMetadata(two, two.size()), "non-load segment overlaps record despite valid load");
		two[0x80] = 0;
		two[0x81] = 3;
		Check(Name, !Loader::ReadAuthorityMetadata(two, two.size()), "non-load segment starts beyond file bounds");
	}
	{
		// A complete table/record supplied beyond the bounded prefix must still
		// be unknown. Without the cap this otherwise valid fixture would parse.
		std::vector<uint8_t> beyond(0x10070);
		std::copy(Header.begin(), Header.end(), beyond.begin());
		beyond[56] = 0x92;
		beyond[57] = 4; // 1170 program headers: record at 0x10030.
		std::copy(Segment.begin(), Segment.end(), beyond.begin() + 0x40);
		beyond[0x49] = 0;
		beyond[0x4a] = 2; // PT_LOAD at 0x20000, within declared input length.
		std::copy(Record.begin(), Record.end(), beyond.begin() + 0x10030);
		Check(Name, !Loader::ReadAuthorityMetadata(beyond, 0x20010), "header recognition exceeded its 64 KiB budget");
	}
	const auto self = Self();
	const auto wrapped = Loader::ReadAuthorityMetadata(self, self.size());
	Check(Name, wrapped && wrapped->authority_id == found->authority_id && wrapped->raw_record == Record &&
	            wrapped->file_offset == 0xb8 && wrapped->provenance == Provenance::SELFContainer,
	      "SELF record must follow the embedded program table with container provenance");
	{
		auto update = self;
		update[7] = 0x32;
		update[9] = 7;
		update[26] = 0x52;
		update[0x50] = 0; // Actual updatemode ET_FE00, parser only (existing loader supports FE10/FE18).
		Check(Name, Loader::ReadAuthorityMetadata(update, update.size()).has_value(), "known update SELF variant rejected");
	}
	for (size_t offset: {size_t {4}, size_t {12}, size_t {16}, size_t {24}, size_t {26}, size_t {28}}) {
		auto bad = self;
		bad[offset] ^= 0x80;
		Check(Name, !Loader::ReadAuthorityMetadata(bad, bad.size()), "malformed SELF wrapper accepted");
	}
	{
		auto bad = self;
		bad[0x28] = 0xb8;
		bad[0x29] = 0;
		Check(Name, !Loader::ReadAuthorityMetadata(bad, bad.size()), "physical SELF segment overlaps record");
		bad = self;
		std::fill_n(bad.begin() + 0x28, 8, 0xff);
		Check(Name, !Loader::ReadAuthorityMetadata(bad, UINT64_MAX), "SELF physical extent overflow accepted");
		Check(Name, !Loader::ReadAuthorityMetadata(std::span<const uint8_t>(self).first(0xf7), self.size()),
		      "truncated SELF record accepted");
	}

	// Reopening the same Elf64 must clear prior metadata; Save must preserve the
	// entire recognized plain-ELF record, not just its authority ID.
	const auto directory = std::filesystem::temp_directory_path() /
	    ("kyty-authority-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
	Check(Name, std::filesystem::create_directory(directory), "create synthetic fixture directory");
	struct RemoveFixture {
		std::filesystem::path path;
		~RemoveFixture() { std::error_code error; std::filesystem::remove_all(path, error); }
	} cleanup {directory};
	auto write = [&](const char* name, std::span<const uint8_t> bytes) {
		Common::File file;
		Check(Name, file.Create(directory / name), "create synthetic ELF");
		uint32_t written = 0;
		file.Write(bytes.data(), static_cast<uint32_t>(bytes.size()), &written);
		Check(Name, written == bytes.size(), "write complete synthetic ELF");
	};
	write("unrelated-name.elf", plain);
	write("wrapped.self", self);
	constexpr std::array<uint8_t, 3> invalid {0x7f, 'E', 'L'};
	write("invalid.elf", invalid);
	auto absent = plain;
	std::fill(absent.begin() + 0x78, absent.begin() + 0xb8, 0);
	write("SceShellCore.elf", absent);
	Loader::Elf64 image;
	image.Open(directory / "unrelated-name.elf");
	Check(Name, image.IsValid() && image.GetAuthorityMetadata() == found, "Elf64 did not preserve parser snapshot");
	std::array<uint8_t, 8> tail {};
	uint32_t bytes_read = 0;
	image.LoadSegment(reinterpret_cast<uint64_t>(tail.data()), plain.size() - 2, tail.size(), &bytes_read);
	Check(Name, bytes_read == 2 && tail[0] == plain[plain.size() - 2] && tail[1] == plain.back(),
	      "metadata reads must report actual file-backed bytes rather than requested length");
	image.Save(directory / "saved.elf");
	image.Open(directory / "saved.elf");
	Check(Name, image.GetAuthorityMetadata() == found, "plain ELF Save lost header-gap metadata");
	image.Open(directory / "invalid.elf");
	Check(Name, !image.IsValid() && !image.GetAuthorityMetadata(), "invalid reopen retained previous metadata");
	image.Open(directory / "SceShellCore.elf");
	Check(Name, image.IsValid() && !image.GetAuthorityMetadata(), "reopen or filename fabricated authority");
	image.Open(directory / "wrapped.self");
	Check(Name, image.IsValid() && image.GetAuthorityMetadata() == wrapped, "SELF reopen provenance lost");
	image.Save(directory / "unwrapped.elf");
	Check(Name, image.GetAuthorityMetadata() == wrapped, "Save changed original input provenance");
	image.Open(directory / "unwrapped.elf");
	const auto& unwrapped = image.GetAuthorityMetadata();
	Check(Name, image.IsValid() && !image.IsSelf() && unwrapped &&
	            unwrapped->provenance == Provenance::RetainedExtendedInfo && unwrapped->file_offset == 0x78 &&
	            unwrapped->authority_id == wrapped->authority_id && unwrapped->raw_record == wrapped->raw_record &&
	            unwrapped->digest == wrapped->digest && unwrapped == found,
	      "SELF Save/reopen must retain metadata at logical ELF offset with plain-input provenance");
	image.Open(directory / "wrapped.self");
	image.Open(directory / "SceShellCore.elf");
	Check(Name, !image.GetAuthorityMetadata(), "SELF metadata leaked into next input");

	Loader::Program core, shell_ui, unknown;
	core.authority_metadata = found;
	shell_ui.authority_metadata = ui_metadata;
	Check(Name, !unknown.authority_metadata, "new Program must start with unknown authority");
	struct RestoreContext {
		Loader::Program* previous = Loader::RuntimeLinker::GetCurrentProgram();
		~RestoreContext() { Loader::RuntimeLinker::SetCurrentProgram(previous); }
	} restore;
	for (auto* program: {&core, &shell_ui, &unknown, &core}) {
		Loader::RuntimeLinker::SetCurrentProgram(program);
		const auto* current = Loader::RuntimeLinker::GetCurrentProgram();
		Check(Name, current == program && current->authority_metadata == program->authority_metadata,
		      "current-program snapshot crossed executable contexts");
	}
}

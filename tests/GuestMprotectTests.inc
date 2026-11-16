void TestGuestMprotectUnmapped() {
	using namespace Libs::LibKernel::Memory;
	constexpr const char* test = "GuestMprotectUnmapped";
	constexpr size_t page = 0x4000;
	void* address = nullptr;
	CheckOk(test, KernelReserveVirtualRange(&address, 2 * page, 0, page), "reserve without backing");
	CheckOk(test, KernelMprotect(address, page, 0), "retain no-access on an existing reservation");
	uint64_t value = 0;
	Check(test, !TryReadBacking(reinterpret_cast<uint64_t>(address), &value, sizeof(value)),
	      "no-access protection does not create backing");
	Check(test, KernelMprotect(address, page, 3) == Libs::LibKernel::KERNEL_ERROR_ENOMEM,
	      "unbacked reservation must return ENOMEM, not terminate host or fabricate backing");
	CheckOk(test, KernelMapFlexibleMemory(&address, page, 3, 0x10), "back first reservation page");
	*static_cast<volatile uint64_t*>(address) = 0x12345678;
	Check(test, KernelMprotect(address, 2 * page, 1) == Libs::LibKernel::KERNEL_ERROR_ENOMEM,
	      "mixed mapped/unmapped span rejected atomically");
	*static_cast<volatile uint64_t*>(address) = 0x87654321;
	Check(test, *static_cast<volatile uint64_t*>(address) == 0x87654321, "failed protect retained RW backing/data");
	CheckOk(test, KernelMprotect(address, 2 * page, 0), "revoke mixed backed/reserved span");
	CheckOk(test, KernelMprotect(address, page, 3), "restore backed first page only");
	Check(test, *static_cast<volatile uint64_t*>(address) == 0x87654321, "revocation retained existing data");
	CheckOk(test, KernelMunmap(reinterpret_cast<uint64_t>(address), 2 * page), "release mapped and reserved range");
	Check(test, KernelMprotect(address, page, 3) == Libs::LibKernel::KERNEL_ERROR_ENOMEM,
	      "fully unmapped range returns ENOMEM");
	Check(test, KernelMprotect(address, page, 0) == Libs::LibKernel::KERNEL_ERROR_ENOMEM,
	      "PROT_NONE does not create a missing reservation");
}

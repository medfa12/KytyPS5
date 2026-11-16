# KytyPS5 — retail firmware launch

**Use the [`retail-launch` branch](https://github.com/medfa12/KytyPS5/tree/retail-launch).**
It contains the retail ShellUI implementation: native system-module loading,
Mono/AOT and RNPS staging, compositor and controller integration, local runtime
services, and packaged setup tools.

This branch is based on the `ps5-vsh-retail` implementation. Its publication
commit descends from upstream's shared baseline, so GitHub can show the
implementation changes in an ordinary cross-fork comparison.

**Status: experimental.** Historical development configurations reached Home
and selected applications, but a clean retail-only first-user Home/application
flow is not verified. PSN sign-in is not supported by this release workflow.

## What you need

### PC

- Windows 10/11 x64 with an up-to-date Vulkan 1.3 graphics driver.
- Windows PowerShell and Python 3.10 or newer.
- A built/installed copy of this branch, including `kyty_emulator.exe`, its
  runtime DLLs and the `tools` directory.
- A controller, or the emulator's configured keyboard controls.

Install the Python setup dependency:

```powershell
python -m pip install dnfile
```

### Your console files

Start with files already obtained from your own console. An already-jailbroken
console is not enough by itself: the files must match the supported firmware
profile. These instructions do not explain jailbreaking, dumping or decryption.

| Input | What setup expects |
| --- | --- |
| Retail firmware **13.60** | Extracted directories with matching decrypted native executables/modules and managed assembly/AOT companions |
| RNPS bundles | Separately decrypted **Hermes v89 bytecode**, organized by title, with matching firmware manifests/assets |
| Preinstalled fonts | Complete matching `preinst/common/font` tree, including `SST-Bold.otf`; an update's `font.zip` alone was insufficient in testing |
| AGC module | Matching decrypted `libSceAgc.sprx.elf`; setup extracts its register data locally |
| URL configuration | Matching `urlconfig_cache.json` |
| Core/UI records | For profiles that require them, the matching source-bound 136-byte admission records; these are not PSN account credentials |

Keep this firmware layout:

```text
D:/Firmware/13.60/
  system_b/
  system_ex_b/app/
  system_ex_b/common_ex/lib/
  system_ex_b/rnps/apps/
  system_ex_b/vsh_asset/
  preinst/common/font/
  decrypted_modules/libSceAgc.sprx.elf

D:/Firmware/13.60-rnps/
  NPXS40002/application.ps.hbc
  NPXS40141/host.ps.hbc
  ...other required titles and bundles...
```

The RNPS names above are examples, not a complete title list. Setup accepts
`*.ps.hbc` and `*.ps.bundle.hbc` and supplies the unsuffixed `.ps.bundle` path
needed by native callers. Renaming encrypted data does not decrypt it.
Decompiled JavaScript is not a replacement for the bytecode.

An independent community repository exists for RNPS decryption; it is not named
or linked here. Emulation and console jailbreaking are separate activities.

Some required files are absent from an update-only extraction. The current
setup does not provide a general way to obtain the source-bound admission
records. Use a coherent matching set; do not substitute another firmware build
or somebody else's account store. Known module identities are checked, and
unsupported variants are rejected.

## Check, prepare and launch

Run PowerShell in the **installed package directory**. Choose a short, dedicated
writable data path outside both the installation and firmware directories.

```powershell
$setupArgs = @(
    "--firmware", "D:/Firmware/13.60",
    "--data", "C:/Kyty/retail",
    "--rnps-source", "D:/Firmware/13.60-rnps",
    "--url-config", "D:/Firmware/13.60-inputs/urlconfig_cache.json"
)
python -B tools/setup_retail.py @setupArgs --check
```

If the detected profile requires admission records, add them and check again:

```powershell
$setupArgs += @(
    "--core-record", "D:/Firmware/13.60-inputs/core-record.bin",
    "--ui-record", "D:/Firmware/13.60-inputs/ui-record.bin"
)
python -B tools/setup_retail.py @setupArgs --check
```

If your AGC module is elsewhere, add `--agc-module` and its path. Currently the
supported module SHA-256 is
`a12b0f396a6464800a68006d6d64e00349a7b37fc3c1c08b6236e8ca564cbd6d`.

After resolving reported prerequisites:

```powershell
python -B tools/setup_retail.py @setupArgs --launch
```

Omit `--launch` to prepare without starting. Firmware/RNPS inputs stay unchanged;
staging, extracted AGC data, profiles, logs and IPC live under `--data`.
Use a fresh data directory when changing firmware profiles. Navigate manually.

Passing `--check` verifies the implemented prerequisites, not every resource
used by every scene. It is not a guarantee of Home or application compatibility.
Missing-font errors require your matching preinstalled fonts.

## Build from source (Windows)

Install Git, CMake 3.22.1+, Ninja, Visual Studio Build Tools with C++ and
`clang-cl`, Qt 6 (MSVC x64), and `glslang` on PATH. Use an x64 Visual Studio
developer terminal. The proprietary console SDK is not a build requirement.

```powershell
git clone --branch retail-launch --recurse-submodules https://github.com/medfa12/KytyPS5.git
cd KytyPS5
cmake -S . -B _Build/windows -G Ninja `
  -DCMAKE_BUILD_TYPE=Release `
  -DCMAKE_C_COMPILER=clang-cl -DCMAKE_CXX_COMPILER=clang-cl `
  -DCMAKE_PREFIX_PATH="C:/Qt/6.x.x/msvc2022_64"
cmake --build _Build/windows --target launcher
cmake --install _Build/windows --prefix _Build/windows/install
```

Replace the Qt path. For an emulator-only build, configure with
`-DKYTY_BUILD_LAUNCHER=OFF` and build target `kyty_emulator`.
Use `_Build/windows/install` for the setup commands above.

## Distribution

No firmware modules, decrypted RNPS bundles, console fonts, extracted AGC tables,
private keys or account stores are included in this branch's source tree or
setup package. The AGC extractor reads your external module; generated data
remains local. Captured shader regression programs are external test inputs,
not embedded programs.

This publication omits development research notes. It retains license notices,
asset attribution and test-input documentation. The inherited upstream history
is not rewritten; these exclusions describe the published tree and package.

KytyPS5 is independent of Sony Interactive Entertainment and PlayStation.
Use only files you are entitled to use; do not redistribute console content.

## License and credits

[GPL-2.0-only](LICENSE). Based on [Kyty](https://github.com/InoriRus/Kyty)
and [KytyPS5](https://github.com/kytyps5/kytyps5).
The original [Kyty MIT notice](LICENSES/Kyty-MIT.txt) and third-party notices
remain applicable. Generated UI assets are documented in
[assets/GENERATED_ASSETS.md](assets/GENERATED_ASSETS.md).

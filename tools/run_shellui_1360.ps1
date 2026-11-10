param(
    [string]$LogName = 'chainSUI',
    [string]$EmulatorPath = '',
    # Packaged setup supplies an external root for logs, IPC and writable data.
    [string]$RuntimeRoot = '',
    [string]$AgcRegisterData = $env:KYTY_AGC_REGISTER_DATA,
    [string]$SystemDataPath = '',
    [string]$FirmwareRoot = $env:KYTY_FIRMWARE_ROOT,
    [string]$ManagedStage = '',
    [ValidateSet('auto', 'active', 'alternate')][string]$LauncherProfile = 'auto',
    [string]$RnpsSource = '',
    # Retained Core/UI records used by source-bound native AppDb admission.
    [string]$CoreAuthRecord = $env:KYTY_APPDB_CORE_AUTH_RECORD,
    [string]$UiAuthRecord = $env:KYTY_APPDB_UI_AUTH_RECORD,
    # Stage and save the exact argv for inspection, without starting an emulator.
    [switch]$PrepareOnly,
    # Optional same-version asset override; otherwise use the selected firmware.
    [string]$VshAssetPath = $env:KYTY_VSH_ASSET_PATH,
    [string]$UrlConfigReference = $env:KYTY_URLCONFIG_REFERENCE,
    [string]$NotificationReference = $env:KYTY_NOTIFICATION_REFERENCE,
    [ValidateSet('Silent', 'Console', 'File')][string]$PrintfDirection = 'File',
    [ValidateSet('error', 'critical', 'warning', 'message', 'info', 'debug')][string]$MonoLogLevel = 'debug',
    [string]$ShaderLogFolder = '',
    [ValidateRange(0, 86400)][int]$Seconds = 0,
    [switch]$Visible,
    [ValidateRange(0, 86400)][int]$IdleSeconds = 0,
    # Kill the emulator when neither log stream has been written for this many
    # seconds while the process is still alive (the observed freeze signature:
    # CPU stays pinned, output stops completely). 0 disables.
    [ValidateRange(0, 86400)][int]$StallSeconds = 0,
    # Bounded relaunch count after a stall; each attempt's streams are rotated
    # to <name>.stall<N> instead of being truncated by the relaunch.
    [ValidateRange(0, 20)][int]$Restarts = 0,
    # Stop the run as soon as this regex appears in the emulator log (requires
    # -StallSeconds so the log tail is being read). Experiment convenience:
    # ends a diagnostic run right after the event of interest.
    [string]$StopPattern = '',
    # Opt in to the RNPS decrypt-replay gate (KYTY_RNPS_REPLAY=1 in the
    # emulator's *host* environment). Without it RnpsBundle::VerifyAndDecrypt
    # fails closed with 0x80940001, so the guest never sees a plaintext host
    # bundle and Hermes rejects the RNPSHEDR file outright. Explicit switch
    # instead of an ambient env var so each run records its own configuration.
    [switch]$RnpsReplay,
    # Opt in to named event-flag tracing (KYTY_EVENTFLAG_TRACE in the emulator
    # host environment). Value is "1"/"*" for every named flag, or a
    # comma-separated list of name substrings, e.g.
    #   -EventFlagTrace 'SceShellUIBootManager,SceBootStatusFlags'
    # Off by default: tracing a hot flag floods the ~700 KB/s guest stream.
    [string]$EventFlagTrace = '',
    # Opt in to the real base_dll registry path (KYTY_RNPS_REAL_REGISTRY=1),
    # which makes the HLE Hermes host publish the real base_dll module registry
    # instead of a synthetic RN namespace, then drive AppRegistry.runApplication
    # once. Implies -RnpsReplay (it needs plaintext bundles). NOTE: the HLE
    # Hermes host is currently unreachable in this configuration (the co-hosted
    # real libhermes satisfies the guest's NIDs, so HermesVmExecute is never
    # called), so this switch is unvalidated and off by default; see
    # docs/rnps-1360-log.md.
    [switch]$RnpsRealRegistry,
    # Allow a foreground user session (omit the -NoForegroundUser guest arg).
    # With the arg, ForegroundUserManager.DoFirstLogin collapses into
    # CloseForegroundSession and the OpenSession event that opens
    # pshomeui:main never fires, so the home app is never launched.
    [switch]$ForegroundUser,
    # Opt in to the persistent user store (KYTY_USER_STORE guest env pointing
    # at _SystemData/priv/user/users.json): UserService boots logged-out with
    # list-based login state instead of fabricating the Config user.
    [switch]$UserStore,
    # Optional isolated profile store; implies -UserStore.
    [string]$UserStorePath = '',
    # Opt in to the first-boot experiment: seed setup incomplete and stage a
    # hash-checked Legacy copy that skips its automatic startup-complete write.
    # Guest registry writes remain enabled. Requires isolated system/user data.
    [switch]$FirstBoot,
    # Enable the GC suspend/resume trace (KYTY_GC_THREAD_TRACE=1 guest env),
    # including the near-stack-bottom margin probe in libKernel.cpp.
    [switch]$GcThreadTrace,
    # Log native IPMI server registrations without invoking their handlers.
    [switch]$TraceNativeIpmi,
    # Load only NpCommon's audited callback-driven ownership export family.
    [switch]$NativeNpOwnership,
    # Opt in to the audited native queue/task family; includes native ownership.
    [switch]$NativeNpWorkQueue,
    # Separate audited callout lifecycle; includes queue/task and ownership.
    [switch]$NativeNpCallout,
    # Separate empty HTTP-pool lifecycle; includes callout, queue and ownership.
    [switch]$NativeNpHttpPool,
    # Regular native72: ownership/workqueue/callout/empty-pool/dispatcher plus
    # complete EventQueue lifecycle. Does not imply NP service readiness or PSN access.
    [switch]$NativeNpStartup,
    # Matching alternate provider's audited 54-export family; excludes dispatcher56.
    [switch]$NativeNpAlternateStartup,
    # Separate native56 dispatcher pair; requires the parent's runtime bridge.
    [switch]$NativeNpDispatcher,
    # Audited, once-only primary Libc startup transition before module initializers.
    [switch]$NativeLibcStartup,
    # Isolated fresh native SSL init/stats/term; exits before ShellUI entries.
    [switch]$NativeSslSmoke,
    # Experimental hash-pinned native SSL routing; continues ordinary boot after real I/S init.
    [switch]$NativeSsl,
    # Use the host adapter and real internet sockets/HTTP transport.
    [switch]$HostNetwork,
    # Local persistent/guest profiles and NP state queries; no online session.
    [switch]$LocalAccountHle,
    [string]$WorkDir = '',
    [switch]$OfflineDatabases,
    [switch]$NoDatabases,
    [switch]$DevelopmentApps,
    # Opt-in genuine built-in catalog records; does not confer PSN access.
    [ValidateSet('NPXS40037', 'NPXS40047')][string[]]$SystemApps = @(),
    # Extracted app directories containing sce_sys/param.json (native or web apps).
    [string]$ExternalAppRoot = '',
    # Exercise the same-session application backend inside the co-hosted VSH boot.
    [switch]$VshRuntime,
    # Kill and relaunch a run whose guest stream exceeds this many MB. The
    # graphics-debug dump makes _kyty.txt grow by gigabytes while the guest is
    # parked in the compositor/render loop (one stalled boot wrote 2.4 GB in an
    # hour); an unbounded log is not progress. 0 disables.
    [ValidateRange(0, 100000)][int]$MaxLogMB = 0,
    # Start tools/monitor_shellui_run.ps1 alongside the run and keep its log.
    # The watcher is read-only: it never starts, stops or writes to the
    # emulator, and its only writes are crash artefacts (in _Runs/<name>/crash)
    # after a non-clean host exit.
    [switch]$Monitor,
    # Watch the emulator window title's frame counter and kill the run when it
    # stops advancing. A boot parked in the compositor/shell loop keeps writing
    # log bytes (CP packets, equeue polling, Mono refcount narration) while the
    # frame number is frozen, so log silence alone cannot see that park.
    [switch]$FrameWatch,
    [ValidateRange(0, 86400)][int]$FrameStallSeconds = 90,
    [ValidateRange(0, 86400)][int]$FlipParkSeconds = 10,
    [ValidateRange(0, 86400)][int]$NoveltyStallSeconds = 120,
    [switch]$FrameDump,
    [int]$FrameDumpEvery = 1,
    [switch]$NoGraphicsDebugDump
)
# $WorkDir: separate emulator working directory for parallel boots. It sets the
# process CWD, which is what the CWD-relative guest mounts resolve against
# (_TempData/<title> -> /temp, /temp0; _TempData/empty_root -> /). Defaults to
# the workspace. Where the emulator stream is written is independent of this:
# every run writes $workspace/_Runs/<LogName>/_kyty.txt.
# 13.60 ShellUI chain (voldemort tools/run_shellui.ps1 parity).
# Stages the managed image, then boots the co-hosted chain with voldemort's
# mount set mapped onto the 13.60 dump:
#   /app0                     = --game dir (system_b/sys, voldemort parity)
#   /app0/psm                 = fw NPXS40087/psm (native PSM resources)
#   /app0/psm/Application     = staged managed assemblies (app.exe, ...)
#   /kyty/common_ex/lib       = staged managed common (Mono search dir)
#   libmonosgen-2.0.sprx      = co-hosted for symbols (resolves the
#                               libmonosgen_v2 stubs that killed PSM before)
$workspace = Split-Path -Parent $PSScriptRoot
if ($RuntimeRoot) { $workspace = [System.IO.Path]::GetFullPath($RuntimeRoot) }
if ([string]::IsNullOrWhiteSpace($FirmwareRoot)) {
    throw 'Select an extracted firmware directory with -FirmwareRoot or KYTY_FIRMWARE_ROOT.'
}
$fw = [System.IO.Path]::GetFullPath($FirmwareRoot)
if (-not (Test-Path -LiteralPath "$fw/system_b" -PathType Container) -or
    -not (Test-Path -LiteralPath "$fw/system_ex_b" -PathType Container)) {
    throw 'FirmwareRoot must contain system_b and system_ex_b.'
}
if ([string]::IsNullOrWhiteSpace($VshAssetPath)) {
    $VshAssetPath = "$fw/system_ex_b/vsh_asset"
}
$VshAssetPath = [System.IO.Path]::GetFullPath($VshAssetPath)
if (-not (Test-Path -LiteralPath $VshAssetPath -PathType Container)) {
    throw "VshAssetPath is not a directory: $VshAssetPath"
}
if ($LauncherProfile -eq 'auto') {
    $detectedProfile = & python -B "$PSScriptRoot/prepare_shellui_managed_1360.py" --fw-root $fw --detect-profile
    if ($LASTEXITCODE -ne 0 -or $detectedProfile -notin @('active', 'alternate')) {
        throw 'Firmware profile detection failed. Supply a supported extracted 13.60 dump.'
    }
    $LauncherProfile = $detectedProfile
    Write-Output "Detected firmware launcher profile: $LauncherProfile"
}
if ([bool]$CoreAuthRecord -ne [bool]$UiAuthRecord) {
    throw 'CoreAuthRecord and UiAuthRecord must be supplied together.'
}
if ($LauncherProfile -eq 'alternate' -and $VshRuntime -and -not $CoreAuthRecord) {
    throw 'Alternate VSH requires CoreAuthRecord and UiAuthRecord for native AppDb startup. See src/loader/bootCredentialAdmission.md.'
}
foreach ($record in @($CoreAuthRecord, $UiAuthRecord)) {
    if ($record -and (-not (Test-Path -LiteralPath $record -PathType Leaf) -or
        (Get-Item -LiteralPath $record).Length -ne 136)) {
        throw "Expected a retained 136-byte auth record: $record"
    }
}
if ($CoreAuthRecord) {
    $CoreAuthRecord = [System.IO.Path]::GetFullPath($CoreAuthRecord)
    $UiAuthRecord = [System.IO.Path]::GetFullPath($UiAuthRecord)
}
if ($NativeNpAlternateStartup) {
    if ($NativeNpStartup -or $NativeNpDispatcher) {
        throw '-NativeNpAlternateStartup cannot be combined with original startup or dispatcher selection.'
    }
    if ($LauncherProfile -ne 'alternate') {
        throw '-NativeNpAlternateStartup requires the matching alternate launcher profile.'
    }
    $NativeNpHttpPool = $true
}
$nativeNpEventQueue = [bool]$NativeNpStartup
if ($nativeNpEventQueue) { $NativeNpDispatcher = $true }
if ($NativeNpDispatcher) { $NativeNpStartup = $true }
if ($NativeNpStartup) { $NativeNpHttpPool = $true }
if ($NativeNpHttpPool) { $NativeNpCallout = $true }
if ($NativeNpCallout) { $NativeNpWorkQueue = $true }
if ($NativeNpWorkQueue) { $NativeNpOwnership = $true }
if ($LauncherProfile -eq 'alternate' -and
    ($FirmwareRoot -eq '' -or $ManagedStage -eq '' -or $SystemDataPath -eq '')) {
    throw 'Alternate boot requires explicit FirmwareRoot, ManagedStage and isolated SystemDataPath.'
}
Write-Output "VSH assets: /system_ex/vsh_asset=$VshAssetPath"
$stage = "$workspace/_ShellUIManaged"
if ($ManagedStage -ne '') { $stage = [System.IO.Path]::GetFullPath($ManagedStage) }
$systemData = "$workspace/_SystemData"
if ($SystemDataPath -ne '') { $systemData = [System.IO.Path]::GetFullPath($SystemDataPath) }
if ($EmulatorPath -eq '') { $EmulatorPath = "$workspace/_Build/windows/kyty_emulator.exe" }
# Fail before creating a stage, log or store when the required URL cache is
# absent. Alternate firmware must never silently inherit the active workspace's
# cache; an explicit reference is the caller's provenance responsibility.
$urlCandidates = @((Join-Path $systemData 'priv/cache/urlconfig/urlconfig_cache.json'))
if ($LauncherProfile -ne 'alternate') {
    $urlCandidates += (Join-Path $workspace '_SystemData/priv/cache/urlconfig/urlconfig_cache.json')
}
if ($UrlConfigReference) { $urlCandidates += $UrlConfigReference }
$urlAvailable = @($urlCandidates | Where-Object { Test-Path -LiteralPath $_ -PathType Leaf }).Count -gt 0
if (-not $urlAvailable) {
    throw 'RNPS URL configuration is missing. Provide a matching urlconfig_cache.json in SystemDataPath or via -UrlConfigReference. No staging was started.'
}
$externalApps = @()
if ($ExternalAppRoot -ne '') {
    $ExternalAppRoot = (Resolve-Path -LiteralPath $ExternalAppRoot -ErrorAction Stop).Path
    $titles = @{}
    $externalApps = @(Get-ChildItem -LiteralPath $ExternalAppRoot -Directory | Sort-Object Name | ForEach-Object {
        $metadata = Join-Path $_.FullName 'sce_sys'
        $descriptor = Join-Path $metadata 'param.json'
        if (Test-Path -LiteralPath $descriptor -PathType Leaf) {
            $param = Get-Content -LiteralPath $descriptor -Raw | ConvertFrom-Json
            $title = $param.titleId
            if ($title -cnotmatch '^[A-Z]{4}[0-9]{5}$' -or $titles.ContainsKey($title)) {
                throw "Invalid or duplicate external title: $title"
            }
            if (-not (Test-Path -LiteralPath "$metadata/icon0.png" -PathType Leaf)) {
                throw "Missing external app icon: $metadata"
            }
            $titles[$title] = $true
            [pscustomobject]@{ Title = $title; Root = $_.FullName; Metadata = $metadata }
        }
    })
    if ($externalApps.Count -eq 0) { throw "No extracted apps found: $ExternalAppRoot" }
}
if ($UserStorePath -ne '') { $UserStore = $true }
if ($UserStorePath -eq '') { $UserStorePath = "$systemData/priv/user/users.json" }
$UserStorePath = [System.IO.Path]::GetFullPath($UserStorePath)
# AF_UNIX includes the backing directory in its 108-byte endpoint limit.
# Nested profile paths and descriptive log names can otherwise abort PSM before
# Home. Give each launch a fresh short endpoint directory independent of both.
$ipcData = "$workspace/_TempData/ipc/$([Guid]::NewGuid().ToString('N').Substring(0, 8))"
# Emulator guest-printf stream (--printf-output-file). Default emulator behavior
# is "_kyty.txt" relative to the process CWD (= the repo root), which leaves one
# ever-growing, git-untracked 100s-of-MB file that every run silently
# overwrites. Pin it per run instead, next to that run's guest log, so runs stay
# separable and the workspace root stays clean. The file is truncated on open.
$emulatorLog = "$workspace/_Runs/$LogName/_kyty.txt"
New-Item -ItemType Directory -Path (Split-Path -Parent $emulatorLog) -Force | Out-Null

$prepareArgs = @('-B', "$PSScriptRoot/prepare_shellui_managed_1360.py", '--fw-root', $fw,
    '--stage', $stage, '--launcher-profile', $LauncherProfile)
if ($RnpsSource -ne '') { $prepareArgs += @('--rnps-source', [System.IO.Path]::GetFullPath($RnpsSource)) }
& python @prepareArgs
if ($LASTEXITCODE -ne 0) { throw 'Managed image staging failed' }
$preinstRoot = "$fw/preinst"
if (-not (Test-Path -LiteralPath "$preinstRoot/common/font" -PathType Container)) {
    $preinstRoot = "$stage/preinst"
}
$commonStage = "$stage/common"
if ($FirstBoot) {
    if ($SystemDataPath -eq '' -or -not $UserStore) {
        throw 'FirstBoot requires an isolated SystemDataPath and UserStorePath.'
    }
    $commonStage = "$workspace/_Runs/$LogName/firstboot-common"
    & python -B "$PSScriptRoot/prepare_shellui_first_boot_1360.py" --source "$stage/common" --destination $commonStage --profile $LauncherProfile
    if ($LASTEXITCODE -ne 0) { throw 'First-boot image staging failed' }
}
# Per-title RNPS overlays (13.60 bundles are encrypted in the dump; a title in
# staged_titles.json has decrypted bundles staged under $stage/rnps/apps/<title>).
# Mount each one over the fw path so the guest's RNPS loader reads the plaintext
# payload while every other file still comes from the read-only dump.
$stagedTitles = @()
$stagedTitlesFile = "$stage/rnps/staged_titles.json"
if (Test-Path $stagedTitlesFile) {
    # Windows PowerShell emits the JSON array as one pipeline object.
    $stagedTitles = @((Get-Content $stagedTitlesFile -Raw | ConvertFrom-Json) | ForEach-Object { $_ })
}
New-Item -ItemType Directory -Path $systemData -Force | Out-Null
New-Item -ItemType Directory -Path "$systemData/user" -Force | Out-Null
if ($SystemDataPath -ne '') {
    # Explicit experiment roots: create empty backing, never copy shared user data.
    New-Item -ItemType Directory -Path "$systemData/data" -Force | Out-Null
    New-Item -ItemType Directory -Path "$systemData/original_user" -Force | Out-Null
    Write-Output "Isolated store: /system_data=$systemData; /user=$systemData/user"
    Write-Output "Isolated fallback roots: /data=$systemData/data; /original_user=$systemData/original_user"
}
New-Item -ItemType Directory -Path "$systemData/common_temp" -Force | Out-Null
New-Item -ItemType Directory -Path "$systemData/priv" -Force | Out-Null
New-Item -ItemType Directory -Path "$systemData/priv/cache/urlconfig" -Force | Out-Null
New-Item -ItemType Directory -Path "$systemData/priv/mms" -Force | Out-Null
# Native SQLite creates its per-process record file here. Provision the writable
# directory, leaving record contents and all database creation to the guest.
New-Item -ItemType Directory -Path "$systemData/priv/mms/nobackup/sqlite" -Force | Out-Null
if (-not $NoDatabases -and -not (Test-Path "$systemData/priv/mms/notification2.db")) {
    if ($NotificationReference) {
        if (-not (Test-Path -LiteralPath $NotificationReference -PathType Leaf)) {
            throw 'NotificationReference must be an existing database file.'
        }
        Copy-Item -LiteralPath $NotificationReference -Destination "$systemData/priv/mms/notification2.db"
    }
}
$urlArgs = @{SystemDataPath=$systemData; WorkspacePath=$workspace; ReferencePath=$UrlConfigReference}
if ($LauncherProfile -eq 'alternate') { $urlArgs.NoWorkspaceFallback = $true }
& "$PSScriptRoot/prepare_shellui_urlconfig.ps1" @urlArgs
New-Item -ItemType Directory -Path $ipcData -Force | Out-Null
# Offline SQLite readers (voldemort prepare_shellui_databases.py parity): the
# managed Accessor.Db assembly is loaded by ReactNativeApps/AppInstUtilWrapper
# and reads priv/mms/*.db directly while the AppDb service is unimplemented.
if ($NoDatabases) {
    Write-Output "Offline databases: SKIPPED (-NoDatabases; $systemData/priv/mms/*.db may be stale)"
} else {
    if ($OfflineDatabases) {
        Write-Output 'Offline databases: preparing (always prepared; -OfflineDatabases is a compat no-op)'
    } else {
        Write-Output 'Offline databases: preparing'
    }
    $dbArgs = @('-B', "$PSScriptRoot/prepare_shellui_databases_1360.py",
        '--managed', $stage, '--firmware', $fw, '--user-id', '268435457',
        '--destination', "$systemData/priv/mms")
    if ($ExternalAppRoot -ne '') { $dbArgs += @('--external-app-root', $ExternalAppRoot) }
    if ($DevelopmentApps -or $FirstBoot) { $dbArgs += '--development-apps' }
    foreach ($title in $SystemApps) { $dbArgs += @('--system-app', $title) }
    if ($UserStore) { $dbArgs += @('--user-store', $UserStorePath) }
    & python @dbArgs
    if ($LASTEXITCODE -ne 0) { throw 'Offline ShellUI database preparation failed' }
}
# Stale AF_UNIX endpoints from a reused run name would make the server's bind fail.
Get-ChildItem -LiteralPath $ipcData -Force | Remove-Item -Force -ErrorAction Stop

$runArgs = @('--game', "$fw\system_b\sys\SceSysCore.elf",
    '--co-elf', "$fw\system_b\vsh\SceShellCore.elf",
    '--co-elf', "$fw\system_b\sys\AgcCompositor.elf",
    '--mount', "/system=$fw\system_b",
    '--mount', "/system_ex=$fw\system_ex_b",
    '--mount', "/preinst=$preinstRoot")
$runArgs += @('--devkit-activation-days', '-1', '--pup-expiration-days', '-1')
$runArgs += @('--user-id', '268435457')
if ($VshRuntime) { $runArgs += '--vsh-session' }
if (-not $ForegroundUser) { $runArgs += @('--guest-arg', '-NoForegroundUser') }
$runArgs += @('--guest-env', 'MONO_THREADS_SUSPEND=preemptive')
$runArgs += @('--guest-env', "MONO_LOG_LEVEL=$MonoLogLevel")
$runArgs += @('--guest-env', 'KYTY_SKIP_ENCRYPTED_OVERLAYS=1')
if ($LocalAccountHle) {
    $runArgs += @('--guest-env', 'KYTY_NP_LOCAL_ACCOUNT_HLE=1')
    Write-Output 'Local-account HLE: ENABLED (persistent/guest profiles and signed-out queries; no online session)'
}
if ($UserStore) {
    New-Item -ItemType Directory -Path "$systemData/priv/user" -Force | Out-Null
    $runArgs += @('--guest-env', "KYTY_USER_STORE=$UserStorePath")
}
if ($GcThreadTrace) { $runArgs += @('--guest-env', 'KYTY_GC_THREAD_TRACE=1') }
if ($TraceNativeIpmi) { $runArgs += @('--guest-env', 'KYTY_TRACE_NATIVE_IPMI=1') }
if ($SystemDataPath -ne '') {
    $runArgs += @('--mount', "/data=$systemData/data",
        '--mount', "/original_user=$systemData/original_user")
}
$runArgs += @('--param-json', "$fw\system_ex_b\app\NPXS40087\sce_sys\param.json",
    '--mount', "/system_data=$systemData",
    '--mount', "/user=$systemData/user",
    '--mount', "/system_tmp=$ipcData",
    '--co-elf', "$stage\native\eboot.bin",
    '--co-elf', "$fw\system_b\common\lib\libSceJemspace.sprx",
    '--co-elf', "$fw\system_ex_b\common_ex\lib\libScePsm.sprx",
    '--co-elf', "$stage\common\libmonosgen-2.0.sprx",
    '--co-elf', "$fw\system_ex_b\common_ex\lib\libSceJsi-0.70.sprx",
    # ps5-vsh (2026-10-22): libhermes MUST be placed before libNativeExtensions
    # relocates. Measured order in ab_real: libNativeExtensions placed at line
    # 1019742 and its libhermes imports bound to zero stubs at 1021590, but
    # libhermes-0.11.sprx was not placed until line 2756081 - 1.7M lines later.
    # The GOT is fixed at bind time, so loading it afterwards cannot help; that
    # is why the k_cohost_libs entry for libhermes-0.11_v1 alone changed
    # nothing. rnpsreal1 never placed it at all.
    '--co-elf', "$stage\common\libhermes-0.11.sprx",
    # ps5-vsh (2026-10-22): libSceRnpsBundle carries K9DCgmMthBQ, the bundle
    # registry initialiser that _sceRnpsBundleRegistryInit() forwards to. Our
    # HLE RnpsBundle library does not export it, so libNativeExtensions bound
    # it to a zero stub and the RNPS bundle registry was never created:
    #   Unresolved import stub called [79]: K9DCgmMthBQ[RnpsBundle_v1]
    # Co-loading the real module lets the linker fill that single gap; the
    # eight NIDs we do implement stay on our HLE replay.
    '--co-elf', "$stage\common\libSceRnpsBundle.sprx",
    # 13.60 eboot registers native TurboModule factories before managed startup.
    # Load their real registry first; late DllImport cannot replay registrations
    # lost to the unresolved 9h4kSOsmZAE import (including Control Center).
    '--co-elf', "$stage\common\libNativeExtensions.sprx",
    '--co-elf', "$fw\system_b\common\lib\libSceCloudMessaging.sprx",
    '--co-elf', "$fw\system_b\common\lib\libSceNotification.sprx",
    '--co-elf', "$fw\system_ex_b\common_ex\lib\libScePsmUtil.sprx",
    '--co-elf', "$fw\system_b\common\lib\libSceRnpsAppMgr.sprx",
    '--co-elf', "$fw\system_b\common\lib\libSceIme.sprx",
    '--co-elf', "$fw\system_b\common\lib\libSceImeBackend.sprx",
    '--co-elf', "$fw\system_b\common\lib\libSceImeDialog.sprx",
    '--co-elf', "$fw\system_ex_b\common_ex\lib\libSceVshPng.sprx",
    '--co-elf', "$fw\system_ex_b\common_ex\lib\libScePubSubClientShared.sprx")
# ps5-vsh: real daemon services whose HLE presence is only zero-stubs (the
# guest's BootManager reports never arrive without them). Only the two with
# no HLE library of the same name are co-hosted; Mbus/AppInstUtil/
# AbstractStorage/UsbStorage/Ipmi have HLE registrations and stay
# all-or-nothing HLE until those are removed deliberately.
$runArgs += @('--co-elf', "$fw\system_b\priv\lib\libSceLoginMgrServer.sprx",
    '--co-elf', "$fw\system_ex_b\common_ex\lib\libSceAsyncStorageInternal.sprx")
# 13.60 PSM AOT experiment (cf. 4.03: app.exe.sprx + X.dll.sprx load as
# native modules post-init; 13.60's eboot spins waiting for mscorlib's
# AOT registration which nothing loads). Pre-load the two anchor AOT
# images for symbols; shared libs only load+relocate, never spawn entries.
$runArgs += @('--co-elf', "$fw\system_ex_b\common_ex\lib\mscorlib.dll.sprx",
    '--co-elf', "$fw\system_ex_b\app\NPXS40087\psm\Application\app.exe.sprx")
# ps5-vsh: the real C++ exception machinery. The ShellUI's own modules
# (libSceFollyShared, libNativeExtensions, ...) import __cxa_throw /
# __gxx_personality_v0 / _Unwind_Resume / __cxa_* from LibcInternal_v1;
# co-hosting libc makes those imports resolve to Sony's unwinder instead of a
# returning stub (which used to make the guest execute the ud2/int3 padding
# after the throw call as code). Loaded *last* to keep the co-elf slot order
# stable for the modules that host the managed app; the placement is NOT the
# reason for any failure. (An earlier note here blamed the slot shift for mono
# falling back to its compiled-in /common_ex/lib/mono/2.1 assembly dir. That
# was wrong: the real cause was that kyty registered setenv/puts/printf under
# the "libc" HLE identity while every guest module - and this real library -
# declares "LibcInternal", so co-hosting the library rebound PSM's
# setenv("MONO_PATH",...) to Sony's code, whose environ mono's HLE g_getenv
# never reads - it silently returns the compiled-in path. Fixed by also
# registering those NIDs in LIB_DEFINE(InitLibcInternal_1),
# src/libs/libC.cpp; see _TempData/research/gate1_libc_mono.md.)
$runArgs += @('--co-elf', "$fw\system_b\common\lib\libSceLibcInternal.sprx")
if ($NativeNpOwnership) {
    $npCommon = "$fw\system_b\common\lib\libSceNpCommon.sprx"
    if (-not (Test-Path -LiteralPath $npCommon)) { throw "Missing native NP provider: $npCommon" }
    if ($NativeNpStartup) {
        $expectedNpCommon = 'a0df8b8c6b7e7dafce5e1bfd6f31126b61f513462ccbbb5919867f1b187937c3'
        if ((Get-FileHash -Algorithm SHA256 -LiteralPath $npCommon).Hash -ne $expectedNpCommon) {
            throw "Native NP startup requires the audited original 13.60 NpCommon: $npCommon"
        }
        Write-Output "Native NP startup: original NpCommon verified; native service acceptance still required"
    }
    if ($NativeNpAlternateStartup) {
        $expectedNpCommon = '81de809c26e4cd1a73d6268c7e89d1e41cf811397f1ac7be17f70e8e14a26a35'
        if ((Get-FileHash -Algorithm SHA256 -LiteralPath $npCommon).Hash -ne $expectedNpCommon) {
            throw "Alternate NP startup requires the audited matching alternate NpCommon: $npCommon"
        }
        Write-Output 'Native NP startup: alternate NpCommon verified (54-export family); runtime initialization still required'
    }
    $runArgs += @('--co-elf', $npCommon)
}
$runArgs += @('--mount', "/system_ex/rnps=$fw\system_ex_b\rnps",
    '--mount', "/system_ex/vsh_asset=$VshAssetPath",
    '--mount', "/common/font=$preinstRoot/common/font",
    '--mount', "/kyty/common/font=$preinstRoot/common/font",
    '--mount', "/system/common/font2=$fw\system_b\common\font2",
    '--mount', "/system_ex/common_ex/font2=$fw\system_ex_b\common_ex\font2",
    '--mount', "/kyty/common_ex/text_layout=$fw\system_ex_b\common_ex\text_layout",
    '--mount', "/kyty/common=$fw\system_b\common",
    '--mount', "/kyty/priv=$systemData\priv",
    '--mount', "/kyty/common_temp=$systemData\common_temp",
    '--mount', "/kyty/common_ex=$fw\system_ex_b\common_ex",
    '--mount', "/kyty/common_ex/lib=$commonStage",
    '--mount', "/common_ex/lib=$commonStage",
    '--mount', "/app0/psm/Application=$stage\Application",
    '--mount', "/app0/psm/Application/resource=$fw\system_ex_b\app\NPXS40087\psm\Application\resource",
    '--mount', "/app0/psm=$fw\system_ex_b\app\NPXS40087\psm",
    '--mount', "/system_ex/common_ex/lib/mono/aot-cache/amd64=$stage\aot-cache\amd64")
. "$PSScriptRoot/shellui_native_producer_mounts.ps1"
$runArgs += @(Get-ShellUiNativeProducerMounts -FirmwareRoot $fw -CommonStage $commonStage)
foreach ($title in $stagedTitles) {
    $runArgs += @('--mount', "/system_ex/rnps/apps/$title=$stage\rnps\apps\$title")
}
foreach ($app in $externalApps) {
    $runArgs += @('--mount', "/user/app/$($app.Title)=$($app.Root)",
        '--mount', "/user/appmeta/$($app.Title)=$($app.Metadata)")
    Write-Output "External app: /user/app/$($app.Title)=$($app.Root); metadata=/user/appmeta/$($app.Title)"
}
$runArgs += @('--fw-root', $fw, '--printf-direction', $PrintfDirection,
    '--printf-output-file', $emulatorLog)
if (-not $NoGraphicsDebugDump) { $runArgs += '--graphics-debug-dump' }
if ($ShaderLogFolder -ne '') {
    $runArgs += @('--shader-log-folder', [System.IO.Path]::GetFullPath($ShaderLogFolder))
}

function ConvertTo-NativeArgument([string]$Argument) {
    # Start-Process joins arrays without quoting. Preserve whitespace, quotes,
    # and trailing backslashes according to Windows CRT argv parsing rules.
    $escaped = [regex]::Replace($Argument, '(\\*)"', '$1$1\"')
    $escaped = [regex]::Replace($escaped, '(\\+)$', '$1$1')
    return '"' + $escaped + '"'
}

$startOptions = @{
    FilePath = $EmulatorPath
    ArgumentList = (($runArgs | ForEach-Object { ConvertTo-NativeArgument $_ }) -join ' ')
    WorkingDirectory = if ($WorkDir -ne '') { $WorkDir } else { $workspace }
    WindowStyle = 'Hidden'
    PassThru = $true
}
if ($Visible) { $startOptions.WindowStyle = 'Normal' }
if ($PrepareOnly) {
    for ($i = 0; $i -lt $runArgs.Count - 1; $i++) {
        if ($runArgs[$i] -in @('--game', '--co-elf') -and
            -not (Test-Path -LiteralPath $runArgs[$i + 1] -PathType Leaf)) {
            throw "Missing boot image: $($runArgs[$i + 1])"
        }
        if ($runArgs[$i] -eq '--mount') {
            $hostPath = ($runArgs[$i + 1] -split '=', 2)[1]
            if (-not (Test-Path -LiteralPath $hostPath)) { throw "Missing boot mount: $hostPath" }
        }
    }
    $prepared = [ordered]@{
        emulator = $EmulatorPath; working_directory = $startOptions.WorkingDirectory
        firmware = $fw; managed_stage = $stage; launcher_profile = $LauncherProfile
        arguments = $runArgs; rnps_replay = [bool]($RnpsReplay -or $RnpsRealRegistry)
        first_boot = [bool]$FirstBoot; host_network = [bool]$HostNetwork
        native_np_alternate_startup = [bool]$NativeNpAlternateStartup
        native_np_ownership = [bool]$NativeNpOwnership; native_np_workqueue = [bool]$NativeNpWorkQueue
        native_np_callout = [bool]$NativeNpCallout; native_np_http_pool = [bool]$NativeNpHttpPool
        native_np_dispatcher = [bool]$NativeNpDispatcher; native_np_event_queue = $nativeNpEventQueue
        core_auth_record = $CoreAuthRecord; ui_auth_record = $UiAuthRecord
        agc_register_data = $AgcRegisterData
        emulator_started = $false
    }
    $preparedPath = "$workspace/_Runs/$LogName/prepared-launch.json"
    $prepared | ConvertTo-Json -Depth 5 | Set-Content -LiteralPath $preparedPath -Encoding UTF8
    Write-Output "Prepared launch arguments: $preparedPath"
    return
}
if ($AgcRegisterData) {
    if (-not (Test-Path -LiteralPath $AgcRegisterData -PathType Leaf)) {
        throw 'AGC register data missing; run retail firmware setup first'
    }
    $env:KYTY_AGC_REGISTER_DATA = [System.IO.Path]::GetFullPath($AgcRegisterData)
} else {
    Remove-Item Env:\KYTY_AGC_REGISTER_DATA -ErrorAction SilentlyContinue
}
if ($CoreAuthRecord) {
    $env:KYTY_APPDB_CORE_AUTH_RECORD = $CoreAuthRecord
    $env:KYTY_APPDB_UI_AUTH_RECORD = $UiAuthRecord
    Write-Output 'Source-bound Core/UI records: configured (runtime verifies image and record hashes)'
} else {
    Remove-Item Env:\KYTY_APPDB_CORE_AUTH_RECORD -ErrorAction SilentlyContinue
    Remove-Item Env:\KYTY_APPDB_UI_AUTH_RECORD -ErrorAction SilentlyContinue
}
if ($NativeNpAlternateStartup) {
    $env:KYTY_NATIVE_NP_ALTERNATE_STARTUP = '1'
    Write-Output 'Native NP alternate startup: ENABLED (exact matching image; dispatcher excluded)'
} else {
    Remove-Item Env:\KYTY_NATIVE_NP_ALTERNATE_STARTUP -ErrorAction SilentlyContinue
}
if ($nativeNpEventQueue) {
    $env:KYTY_NATIVE_NP_EVENT_QUEUE = '1'
    Write-Output 'Native NP EventQueue: ENABLED (regular72 lifecycle; native initialization still required)'
} else {
    Remove-Item Env:\KYTY_NATIVE_NP_EVENT_QUEUE -ErrorAction SilentlyContinue
}
if ($NativeNpOwnership) {
    $env:KYTY_NATIVE_NP_OWNERSHIP = '1'
    Write-Output 'Native NP ownership: ENABLED (21 isolated NpCommon exports)'
} else {
    Remove-Item Env:\KYTY_NATIVE_NP_OWNERSHIP -ErrorAction SilentlyContinue
    Write-Output 'Native NP ownership: disabled'
}
if ($NativeNpWorkQueue) {
    $env:KYTY_NATIVE_NP_WORKQUEUE = '1'
    Write-Output 'Native NP WorkQueue: ENABLED (isolated queue/task export family)'
} else {
    Remove-Item Env:\KYTY_NATIVE_NP_WORKQUEUE -ErrorAction SilentlyContinue
    Write-Output 'Native NP WorkQueue: disabled'
}
if ($NativeNpCallout) {
    $env:KYTY_NATIVE_NP_CALLOUT = '1'
    Write-Output 'Native NP callouts: ENABLED (51 isolated ownership/queue/callout exports)'
} else {
    Remove-Item Env:\KYTY_NATIVE_NP_CALLOUT -ErrorAction SilentlyContinue
    Write-Output 'Native NP callouts: disabled'
}
if ($NativeNpHttpPool) {
    $env:KYTY_NATIVE_NP_HTTP_POOL = '1'
    Write-Output 'Native NP HTTP pool: ENABLED (54 isolated ownership/queue/callout/pool exports)'
} else {
    Remove-Item Env:\KYTY_NATIVE_NP_HTTP_POOL -ErrorAction SilentlyContinue
    Write-Output 'Native NP HTTP pool: disabled'
}
if ($NativeNpDispatcher) {
    $env:KYTY_NATIVE_NP_DISPATCHER = '1'
    Write-Output 'Native NP dispatcher: requested (native56; runtime transport installation required)'
} else {
    Remove-Item Env:\KYTY_NATIVE_NP_DISPATCHER -ErrorAction SilentlyContinue
}
if ($NativeSsl -and $NativeSslSmoke) {
    throw '-NativeSsl and -NativeSslSmoke are mutually exclusive'
}
if ($HostNetwork) {
    $env:KYTY_NETWORK_HOST = '1'
    Write-Output 'Host internet: ENABLED'
} else {
    Remove-Item Env:\KYTY_NETWORK_HOST -ErrorAction SilentlyContinue
}
if ($NativeSsl) {
    $env:KYTY_NATIVE_SSL = '1'
    Write-Output 'Native SSL: ENABLED (experimental pinned provider, normal boot; not full SSL service closure)'
} else {
    Remove-Item Env:\KYTY_NATIVE_SSL -ErrorAction SilentlyContinue
    Write-Output 'Native SSL: disabled'
}
if ($NativeSslSmoke) {
    $env:KYTY_NATIVE_SSL_SMOKE = '1'
    Write-Output 'Native SSL smoke: ENABLED (isolated session, native init/stats/term)'
} else {
    Remove-Item Env:\KYTY_NATIVE_SSL_SMOKE -ErrorAction SilentlyContinue
    Write-Output 'Native SSL smoke: disabled'
}
if ($NativeLibcStartup -or $NativeSslSmoke -or $NativeSsl) {
    $env:KYTY_NATIVE_LIBC_STARTUP = '1'
    Write-Output 'Native Libc primary startup: ENABLED (audited provider, once before initializers)'
} else {
    Remove-Item Env:\KYTY_NATIVE_LIBC_STARTUP -ErrorAction SilentlyContinue
    Write-Output 'Native Libc primary startup: disabled'
}
if ($RnpsReplay) {
    $env:KYTY_RNPS_REPLAY = '1'
    Write-Output 'RNPS decrypt replay: ENABLED (KYTY_RNPS_REPLAY=1)'
} else {
    Remove-Item Env:\KYTY_RNPS_REPLAY -ErrorAction SilentlyContinue
    Write-Output 'RNPS decrypt replay: disabled (fail-closed 0x80940001)'
}
if ($RnpsRealRegistry) {
    Remove-Item Env:\KYTY_RNPS_REAL_REGISTRY -ErrorAction SilentlyContinue
    Write-Output 'RNPS real registry: DISABLED (unreachable with co-hosted libhermes; switch ignored)'
} else {
    Remove-Item Env:\KYTY_RNPS_REAL_REGISTRY -ErrorAction SilentlyContinue
    Write-Output 'RNPS real base_dll registry: disabled'
}
if ($FirstBoot) {
    $env:KYTY_FIRST_BOOT = '1'
    Write-Output 'First boot: ENABLED (KYTY_FIRST_BOOT=1, initial-setup registry reports 0)'
} else {
    Remove-Item Env:\KYTY_FIRST_BOOT -ErrorAction SilentlyContinue
}
if ($UserStore) {
    Write-Output "User store: ENABLED (KYTY_USER_STORE=$UserStorePath)"
} else {
    Remove-Item Env:\KYTY_USER_STORE -ErrorAction SilentlyContinue
}
if ($EventFlagTrace -ne '') {
    $env:KYTY_EVENTFLAG_TRACE = $EventFlagTrace
    Write-Output "Event-flag tracing: ENABLED (KYTY_EVENTFLAG_TRACE=$EventFlagTrace)"
} else {
    Remove-Item Env:\KYTY_EVENTFLAG_TRACE -ErrorAction SilentlyContinue
    Write-Output 'Event-flag tracing: disabled'
}
if ($FrameDump) {
    $frameDir = "$workspace/_Runs/$LogName/frames"
    New-Item -ItemType Directory -Path $frameDir -Force | Out-Null
    $env:KYTY_FRAME_DUMP_DIR = $frameDir
    $env:KYTY_FRAME_DUMP_EVERY = [string]$FrameDumpEvery
    Write-Output "Frame dump: ENABLED (KYTY_FRAME_DUMP_DIR=$frameDir, EVERY=$FrameDumpEvery)"
} else {
    Remove-Item Env:\KYTY_FRAME_DUMP_DIR -ErrorAction SilentlyContinue
    Remove-Item Env:\KYTY_FRAME_DUMP_EVERY -ErrorAction SilentlyContinue
}
$startOptions.RedirectStandardOutput = "$workspace/_Logs/$LogName.log"
$startOptions.RedirectStandardError = "$workspace/_Logs/$LogName.stderr.log"
New-Item -ItemType Directory -Path "$workspace/_Logs" -Force | Out-Null
$attempt = 0
$exitCode = $null
$result = $null
$monitorProc = $null
$monitorOut = $null
while ($true) {
    # Silent/Console logging never opens the guest file. Preserve an earlier
    # invocation's stream before the first launch so watchdogs see this run only.
    if ($attempt -eq 0 -and (Test-Path -LiteralPath $emulatorLog)) {
        $previousLog = "$emulatorLog.previous-$([Guid]::NewGuid().ToString('N'))"
        Move-Item -LiteralPath $emulatorLog -Destination $previousLog -ErrorAction Stop
    }
    if ($attempt -gt 0) {
        # Preserve each frozen attempt's streams: Start-Process truncates the
        # redirected log and the emulator truncates _kyty.txt on relaunch.
        foreach ($stale in @("$workspace/_Logs/$LogName.log", "$workspace/_Logs/$LogName.stderr.log", "$workspace/_Runs/$LogName/_kyty.txt")) {
            if (Test-Path -LiteralPath $stale) {
                Move-Item -LiteralPath $stale "$stale.stall$attempt" -Force
            }
        }
    }
    $run = Start-Process @startOptions -ErrorAction Stop
    $run.Id | Set-Content -LiteralPath "$workspace/_Logs/$LogName.pid"
    Write-Output ("Shell chain PID={0}, log=_Logs/{1}.log{2}" -f $run.Id, $LogName,
        $(if ($attempt -gt 0) { " (relaunch $attempt/$Restarts after stall-no-log-progress)" } else { '' }))
    if (-not ($Seconds -gt 0 -or $IdleSeconds -gt 0 -or $StallSeconds -gt 0 -or $FrameWatch)) { break }
    # Frame watchdog for this attempt: it reads the emulator window title and
    # kills the emulator when `frame: N` has not advanced for FrameStallSeconds.
    # This is the signal the log-based stall check cannot see: a parked boot
    # keeps emitting GPU/queue narration and Mono refcount chatter.
    $frameProc   = $null
    $frameOffset = 0L
    if ($FrameWatch) {
        $frameLog = "$workspace/_Logs/$LogName.frames.log"
        if (Test-Path -LiteralPath $frameLog) {
            $frameOffset = (Get-Item -LiteralPath $frameLog).Length
        }
        $frameArgs = '-NoProfile -ExecutionPolicy Bypass -File "{0}" -LogName "{1}" -Root "{2}" -StallSeconds {3} -FlipParkSeconds {4} -NoveltyStallSeconds {5} -EmuPid {6} -Follow -KillOnStall' -f `
            (Join-Path $PSScriptRoot 'watch_shellui_frames.ps1'), $LogName, $workspace, $FrameStallSeconds, $FlipParkSeconds, $NoveltyStallSeconds, $run.Id
        $frameProc = Start-Process -FilePath 'powershell.exe' -ArgumentList $frameArgs `
            -PassThru -WindowStyle Hidden
        Write-Output ("Frame watch PID={0}, log=_Logs/{1}.frames.log" -f $frameProc.Id, $LogName)
    }
    # Started after the no-wait break: with none of the wait flags this
    # launcher exits immediately, so a watcher started here would be orphaned
    # by the cleanup below before it could see anything.
    if ($Monitor -and ($null -eq $monitorProc -or $monitorProc.HasExited)) {
        # One long-lived watcher across this launcher's stall relaunches:
        # -Follow keeps it through the pid change and the .stall<N> rotation.
        # Start-Process truncates its redirect target, so a watcher that had
        # to be restarted (because a relaunch took longer than its 15 s follow
        # grace) gets an attempt-scoped file instead of erasing attempt 0.
        $monitorOut = if ($attempt -eq 0 -and $null -eq $monitorOut) {
            "$workspace/_Logs/$LogName.monitor.log"
        } else {
            "$workspace/_Logs/$LogName.monitor.$($attempt + 1).log"
        }
        # One string, not an array: Start-Process does not quote array
        # elements, and $workspace may contain spaces.
        $monitorArgs = '-NoProfile -ExecutionPolicy Bypass -File "{0}" -LogName "{1}" -Root "{2}" -QuietAfter {3} -Follow' -f `
            (Join-Path $PSScriptRoot 'monitor_shellui_run.ps1'), $LogName, $workspace, `
            $(if ($StallSeconds -gt 0) { $StallSeconds } else { 120 })
        $monitorProc = Start-Process -FilePath 'powershell.exe' -ArgumentList $monitorArgs `
            -RedirectStandardOutput $monitorOut `
            -RedirectStandardError ($monitorOut + '.err') `
            -PassThru -WindowStyle Hidden
        Write-Output ("Monitor PID={0}, log=_Logs/{1}" -f $monitorProc.Id, (Split-Path -Leaf $monitorOut))
    }
    # Keep a process handle open so Windows PowerShell can retrieve the exit code
    # even when the guest exits between monitor iterations.
    $processHandle = $run.Handle
    # Observe new startup milestones, not log volume: compositor retries and
    # polling continue even when managed startup has stopped making progress.
    # An idle cutoff is an observation, not a claim that the guest is deadlocked.
    # The stall cutoff is the opposite observation: a frozen boot stops writing
    # guest output for minutes while the process stays alive burning CPU, while
    # a healthy boot prints constantly (mono tracing fires every second). Host
    # idle narration is excluded from the signal - see $kytyLastMeaningful.
    $clock = [System.Diagnostics.Stopwatch]::StartNew()
    $lastProgress = 0.0
    $lastMilestone = '(launch)'
    $seen = New-Object 'System.Collections.Generic.HashSet[string]'
    $reason = 'exited'
    $stallNote = ''
    $stopHit = $false
    $sawOnLaunched = $false
    $reader = $null
    # Stall detection must ignore the emulator's own "I am hung" narration:
    # ps5-vsh: idle-since / blocked-in and the flipPendingNum dump are written
    # to _kyty.txt while every guest thread is parked, which used to keep the
    # stream "alive" and defeat -StallSeconds entirely (rnpsjs2 sat 14 minutes
    # past the freeze). Track only bytes that are not one of those lines.
    $kytyPath = "$workspace/_Runs/$LogName/_kyty.txt"
    $kytyPos = 0
    $kytyLastMeaningful = Get-Date
    try {
        if ($IdleSeconds -gt 0) {
            $stream = [System.IO.File]::Open($startOptions.RedirectStandardOutput,
                [System.IO.FileMode]::Open, [System.IO.FileAccess]::Read,
                ([System.IO.FileShare]::ReadWrite -bor [System.IO.FileShare]::Delete))
            $reader = New-Object System.IO.StreamReader($stream)
        }
        while (-not $run.WaitForExit(1000)) {
            if ($null -ne $reader) {
                while ($null -ne ($line = $reader.ReadLine())) {
                    if ($line -match '^ps5-vsh: relocate-(begin|end)|^Mono: (Assembly |Assembly Loader loaded|Image addref|AOT: (FOUND|image |module )|DllImport |Found as )|UISystem timer started|OnLaunched|frame-dump') {
                        if ($line -match 'OnLaunched') { $sawOnLaunched = $true }
                        if ($seen.Add($line)) {
                            $lastProgress = $clock.Elapsed.TotalSeconds
                            $lastMilestone = $line
                        }
                    }
                }
            }
            if ($StallSeconds -gt 0 -and (Test-Path -LiteralPath $kytyPath)) {
                try {
                    $kfs = [System.IO.File]::Open($kytyPath, [System.IO.FileMode]::Open,
                        [System.IO.FileAccess]::Read,
                        ([System.IO.FileShare]::ReadWrite -bor [System.IO.FileShare]::Delete))
                    $lenNow = $kfs.Length
                    if ($lenNow -lt $kytyPos) { $kytyPos = 0 }   # truncated on relaunch
                    if ($lenNow -gt $kytyPos) {
                        [void]$kfs.Seek($kytyPos, [System.IO.SeekOrigin]::Begin)
                        $ksr = New-Object System.IO.StreamReader($kfs,
                            [System.Text.Encoding]::UTF8, $true, 65536, $true)
                        $chunk = $ksr.ReadToEnd()
                        $ksr.Dispose()
                        $kytyPos = $lenNow
                        # -StallSeconds must survive the emulator's idle
                        # narration: compositor/vblank polling, GPU command
                        # buffer dumps, operator-new traces, the temporary
                        # locale-slot probe and kernel file chatter all keep
                        # writing while every guest thread is parked. Counting
                        # those as progress let one stalled boot write 2.4 GB
                        # of CP packets over 55 M lines and never reach the RNPS
                        # stage. Only lines outside these families reset the
                        # stall clock. Keep this list aligned with the ignore
                        # list in tools/monitor_shellui_run.ps1.
                        $idleNoise = 'idle-since|blocked-in|flipPendingNum|CP packet|CommandProcessor::|GraphicsRender|locations\[|msaa_|aa_mask|max_sample_dist|vertex_count|instance_count|first_vertex|first_instance|cache_policy|event_write_dest|eop_event_type|cache_action|event_index|interrupt_selector|interrupt_context|dst_gpu_addr|cmd_id=|Equeue wait|\scond = 0x|SceSoCPowerWatch|\[Pss\] sceMbus|ps5-vsh trace: OperatorNew|ps5-vsh: SceLocaleInit|ps5-vsh: THROW locale-slot|KernelStat:|KernelFstat:|\sLseek \(pos|\sRead \d+ bytes|\sOpen: |\sfd = 0x|\sfcntl |mutex init|setspecific|vm\.stats|PthreadCondWait'
                        $meaningful = $chunk -replace ('(?m)^.*(?:' + $idleNoise + ').*(\r?\n|$)'), ''
                        if ($meaningful.Trim().Length -gt 0) { $kytyLastMeaningful = Get-Date }
                        if ($StopPattern -and ($chunk -match $StopPattern)) { $stopHit = $true }
                    }
                    $kfs.Dispose()
                } catch {
                    # A monitoring failure must never kill the run by itself.
                }
            }
            if ($MaxLogMB -gt 0 -and (Test-Path -LiteralPath $kytyPath)) {
                $logMB = [Math]::Round((Get-Item -LiteralPath $kytyPath).Length / 1MB, 1)
                if ($logMB -ge $MaxLogMB) {
                    $reason = 'log-runaway'
                    Write-Output ("Guest stream reached {0} MB (cap {1}); stopping." -f $logMB, $MaxLogMB)
                    break
                }
            }
            if ($stopHit) {
                $reason = 'stop-pattern'
                break
            }
            if ($Seconds -gt 0 -and $clock.Elapsed.TotalSeconds -ge $Seconds) {
                $reason = 'time-limit'
                break
            }
            if ($IdleSeconds -gt 0 -and ($clock.Elapsed.TotalSeconds - $lastProgress) -ge $IdleSeconds) {
                $reason = 'no-new-startup-milestones'
                break
            }
            if ($StallSeconds -gt 0) {
                $newest = (Get-Item -LiteralPath $startOptions.RedirectStandardOutput).LastWriteTime
                if ($kytyLastMeaningful -gt $newest) { $newest = $kytyLastMeaningful }
                if (((Get-Date) - $newest).TotalSeconds -ge $StallSeconds) {
                    $reason = 'stall-no-log-progress'
                    # Classify the freeze: a spinning guest burns CPU and keeps its
                    # thread count; a pure wait does neither. Sampled once, here,
                    # so a stuck run reports the evidence it was killed for.
                    try {
                        $stalled = Get-Process -Id $run.Id -ErrorAction Stop
                        $stallCpu = [Math]::Round($stalled.CPU, 1)
                        $stallThreads = $stalled.Threads.Count
                        $stallNote = "cpu=${stallCpu}s threads=${stallThreads}"
                        Write-Output ("Stall observed: {0}" -f $stallNote)
                    } catch {
                        $stallNote = 'unavailable'
                    }
                    break
                }
            }
        }
        if (-not $run.HasExited) {
            Stop-Process -Id $run.Id
            $run.WaitForExit()
        }
        # If the frame watchdog killed this attempt, classify the outcome as a
        # frame stall even though the chain itself exited.
        if ($FrameWatch -and $reason -eq 'exited') {
            $frameLog = "$workspace/_Logs/$LogName.frames.log"
            if (Test-Path -LiteralPath $frameLog) {
                $newFrames = ''
                try {
                    $fs = [System.IO.File]::Open($frameLog, [System.IO.FileMode]::Open,
                        [System.IO.FileAccess]::Read, [System.IO.FileShare]::ReadWrite)
                    try {
                        [void]$fs.Seek($frameOffset, [System.IO.SeekOrigin]::Begin)
                        $newFrames = (New-Object System.IO.StreamReader($fs)).ReadToEnd()
                    } finally {
                        $fs.Dispose()
                    }
                } catch { }
                $hit = ($newFrames -split "`n") | Where-Object { $_ -match 'VERDICT: (frame-stall|flip-park|spin-park)' } | Select-Object -Last 1
                if ($null -ne $hit) {
                    if ($hit -match 'VERDICT: (frame-stall|flip-park|spin-park)') { $reason = $Matches[1] }
                    $stallNote = $hit.Trim()
                }
            }
        }
        if (($reason -eq 'stall-no-log-progress' -or $reason -eq 'log-runaway' -or $reason -eq 'frame-stall' -or $reason -eq 'flip-park' -or $reason -eq 'spin-park') -and $attempt -lt $Restarts) {
            $attempt++
            continue
        }
        # A dud boot (never reached the shell) is worth retrying too: the shell's
        # startup races and roughly half the attempts park before OnLaunched.
        $reachedShell = $false
        if (Test-Path -LiteralPath $kytyPath) {
            $reachedShell = [bool](Select-String -Path $kytyPath -Pattern 'OnLaunched' -Quiet -ErrorAction SilentlyContinue)
        }
        if (-not $reachedShell -and $attempt -lt $Restarts -and $reason -ne 'stop-pattern') {
            Write-Output 'Attempt never reached OnLaunched; relaunching.'
            $attempt++
            continue
        }
        $exitCode = $run.ExitCode
        $result = [ordered]@{
            pid = $run.Id
            reason = $reason
            elapsedSeconds = [Math]::Round($clock.Elapsed.TotalSeconds, 2)
            exitCode = $exitCode
            idleSeconds = [Math]::Round($clock.Elapsed.TotalSeconds - $lastProgress, 2)
            lastMilestone = $lastMilestone
            stallNote = $stallNote
        }
        $result | ConvertTo-Json | Set-Content -LiteralPath "$workspace/_Logs/$LogName.result.json"
        Write-Output ("Shell chain stopped: {0}, elapsed={1}s, exit={2}" -f $reason, $result.elapsedSeconds, $result.exitCode)
    } finally {
        if ($null -ne $reader) { $reader.Dispose() }
        if ($null -ne $frameProc -and -not $frameProc.HasExited) {
            Stop-Process -Id $frameProc.Id -ErrorAction SilentlyContinue
        }
        # A monitoring/read failure must not leave an unbounded test process.
        if (-not $run.HasExited) { Stop-Process -Id $run.Id -ErrorAction SilentlyContinue }
    }
    break
}

if ($Monitor -and $null -ne $monitorProc) {
    # Give the watcher time to observe the final pid/exit code, then stop it.
    # It is strictly read-only, so terminating it cannot affect the run.
    if (-not $monitorProc.HasExited) {
        try { [void]$monitorProc.WaitForExit(4000) } catch { }
    }
    if (-not $monitorProc.HasExited) {
        Stop-Process -Id $monitorProc.Id -ErrorAction SilentlyContinue
    }
    if ($null -ne $monitorOut -and (Test-Path -LiteralPath $monitorOut)) {
        Write-Output ("Monitor tail ({0}):" -f (Split-Path -Leaf $monitorOut))
        Get-Content -LiteralPath $monitorOut -Tail 8 | ForEach-Object { Write-Output ('  ' + $_) }
    }
}

if ($FrameDump) {
    $frameDir = "$workspace/_Runs/$LogName/frames"
    if (Test-Path -LiteralPath $frameDir) {
        & python -c "
import glob, os
from PIL import Image

dump_dir = r'$frameDir'
ppms = sorted(glob.glob(os.path.join(dump_dir, '*.ppm')))
for ppm in ppms:
    png = ppm.replace('.ppm', '.png')
    if not os.path.exists(png):
        try:
            img = Image.open(ppm)
            img.save(png)
            print(f'Converted {os.path.basename(ppm)} ({img.size}) -> {os.path.basename(png)}')
        except Exception as e:
            print(f'Failed {ppm}: {e}')
"
    }
}

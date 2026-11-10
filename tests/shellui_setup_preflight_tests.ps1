$ErrorActionPreference = 'Stop'
$launcher = Join-Path (Split-Path -Parent $PSScriptRoot) 'tools/run_shellui_1360.ps1'
$root = Join-Path $env:TEMP ('kyty-preflight-test-' + [guid]::NewGuid().ToString('N'))
$old = @{}
foreach ($name in @('KYTY_FIRMWARE_ROOT','KYTY_VSH_ASSET_PATH','KYTY_APPDB_CORE_AUTH_RECORD','KYTY_APPDB_UI_AUTH_RECORD','KYTY_URLCONFIG_REFERENCE')) {
    $old[$name] = [Environment]::GetEnvironmentVariable($name, 'Process')
    [Environment]::SetEnvironmentVariable($name, $null, 'Process')
}
try {
    $fw = Join-Path $root 'firmware'
    $stage = Join-Path $root 'stage'
    $store = Join-Path $root 'store'
    New-Item -ItemType Directory -Path "$fw/system_b", "$fw/system_ex_b/vsh_asset" -Force | Out-Null
    $errorMessage = ''
    try {
        & $launcher -FirmwareRoot $fw -LauncherProfile alternate -ManagedStage $stage -SystemDataPath $store -PrepareOnly
    } catch { $errorMessage = $_.Exception.Message }
    if ($errorMessage -notlike '*RNPS URL configuration is missing*No staging was started*') { throw "Unexpected preflight result: $errorMessage" }
    if ((Test-Path -LiteralPath $stage) -or (Test-Path -LiteralPath $store)) { throw 'Missing URL cache created staging or store data' }
    $errorMessage = ''
    try {
        & $launcher -FirmwareRoot $fw -LauncherProfile alternate -ManagedStage $stage -SystemDataPath $store -VshRuntime -PrepareOnly
    } catch { $errorMessage = $_.Exception.Message }
    if ($errorMessage -notlike '*requires CoreAuthRecord and UiAuthRecord*') { throw "Unexpected admission result: $errorMessage" }
    if ((Test-Path -LiteralPath $stage) -or (Test-Path -LiteralPath $store)) { throw 'Missing admission records created staging or store data' }
    Write-Output 'PASS: missing URL cache and admission records fail before staging'
} finally {
    foreach ($name in $old.Keys) { [Environment]::SetEnvironmentVariable($name, $old[$name], 'Process') }
    if (Test-Path -LiteralPath $root) { Remove-Item -LiteralPath $root -Recurse -Force }
}

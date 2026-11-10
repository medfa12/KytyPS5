$ErrorActionPreference = 'Stop'
$launcher = Join-Path (Split-Path -Parent $PSScriptRoot) 'tools/run_shellui_1360.ps1'
$tempRoot = Join-Path ([IO.Path]::GetTempPath()) ('kyty-path-test-' + [guid]::NewGuid().ToString('N'))
$oldFirmware = $env:KYTY_FIRMWARE_ROOT
$oldAssets = $env:KYTY_VSH_ASSET_PATH
function Expect-Failure([hashtable]$Options, [string]$Message) {
    $failed = $false
    try { & $launcher @Options -PrepareOnly } catch {
        if ($_.Exception.Message -notlike $Message) { throw }
        $failed = $true
    }
    if (-not $failed) { throw 'Invalid input unexpectedly reached staging.' }
}
try {
    $env:KYTY_FIRMWARE_ROOT = ''
    $env:KYTY_VSH_ASSET_PATH = ''
    Expect-Failure @{} 'Select an extracted firmware directory*'
    $selected = Join-Path $tempRoot 'selected firmware'
    New-Item -ItemType Directory -Path "$selected/system_b", "$selected/system_ex_b" -Force | Out-Null
    # Valid root structure, missing assets: stop before Python staging or stores.
    $env:KYTY_FIRMWARE_ROOT = $selected
    Expect-Failure @{} "VshAssetPath is not a directory: $selected*"
    $env:KYTY_FIRMWARE_ROOT = Join-Path $tempRoot 'wrong firmware'
    Expect-Failure @{FirmwareRoot=$selected} "VshAssetPath is not a directory: $selected*"
    Expect-Failure @{} 'FirmwareRoot must contain system_b and system_ex_b.'
    $env:KYTY_VSH_ASSET_PATH = Join-Path $tempRoot 'selected assets'
    Expect-Failure @{FirmwareRoot=$selected} "VshAssetPath is not a directory: $tempRoot*selected assets"
    Expect-Failure @{FirmwareRoot=$selected; VshAssetPath=(Join-Path $tempRoot 'explicit assets')} "VshAssetPath is not a directory: $tempRoot*explicit assets"
    Write-Output 'PASS: missing input, environment selection, explicit precedence, root validation, asset overrides'
} finally {
    $env:KYTY_FIRMWARE_ROOT = $oldFirmware
    $env:KYTY_VSH_ASSET_PATH = $oldAssets
    if (Test-Path -LiteralPath $tempRoot) { Remove-Item -LiteralPath $tempRoot -Recurse -Force }
}

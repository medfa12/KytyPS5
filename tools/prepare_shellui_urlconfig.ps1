param(
    [Parameter(Mandatory = $true)][string]$SystemDataPath,
    [string]$WorkspacePath = (Split-Path -Parent $PSScriptRoot),
    [string]$ReferencePath = $env:KYTY_URLCONFIG_REFERENCE,
    [switch]$NoWorkspaceFallback
)
$ErrorActionPreference = 'Stop'
$destination = Join-Path $SystemDataPath 'priv/cache/urlconfig/urlconfig_cache.json'
if (Test-Path -LiteralPath $destination -PathType Leaf) { return }

# This is shared RNPS routing configuration, not user/profile state. Resolve
# sources against the workspace, never the emulator's isolated working directory.
$sources = @()
if (-not $NoWorkspaceFallback) { $sources += (Join-Path $WorkspacePath '_SystemData/priv/cache/urlconfig/urlconfig_cache.json') }
$sources += $ReferencePath
foreach ($source in $sources) {
    if ($source -and (Test-Path -LiteralPath $source -PathType Leaf)) {
        New-Item -ItemType Directory -Path (Split-Path -Parent $destination) -Force | Out-Null
        Copy-Item -LiteralPath $source -Destination $destination
        Write-Output "RNPS URL configuration: $source -> $destination"
        return
    }
}
throw "RNPS URL configuration is missing. Supply a matching urlconfig_cache.json in SystemDataPath or via -UrlConfigReference before launching ShellUI."

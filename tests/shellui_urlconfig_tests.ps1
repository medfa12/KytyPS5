$ErrorActionPreference = 'Stop'
$prepare = Join-Path (Split-Path -Parent $PSScriptRoot) 'tools/prepare_shellui_urlconfig.ps1'
$root = Join-Path $env:TEMP ('kyty-urlconfig-test-' + [guid]::NewGuid().ToString('N'))
$relative = 'priv/cache/urlconfig/urlconfig_cache.json'
try {
    $workspace = Join-Path $root 'workspace'
    $source = Join-Path $workspace "_SystemData/$relative"
    New-Item -ItemType Directory -Path (Split-Path -Parent $source) -Force | Out-Null
    [IO.File]::WriteAllText($source, '{"response":{"scheme":{"default":{"pshomeui":"app:home"}}}}')
    $expected = (Get-FileHash -LiteralPath $source).Hash
    $profile = Join-Path $root 'isolated/sd'
    $missing = Join-Path $root 'absent-reference.json'
    # An unrelated CWD and absent external checkout reproduce the failed runs.
    Push-Location $root
    try { & $prepare -SystemDataPath $profile -WorkspacePath $workspace -ReferencePath $missing } finally { Pop-Location }
    $destination = Join-Path $profile $relative
    if ((Get-FileHash -LiteralPath $destination).Hash -ne $expected) { throw 'Seed bytes differ' }
    [IO.File]::WriteAllText($destination, '{"existing":true}')
    $existing = (Get-FileHash -LiteralPath $destination).Hash
    & $prepare -SystemDataPath $profile -WorkspacePath $workspace -ReferencePath $missing
    if ((Get-FileHash -LiteralPath $destination).Hash -ne $existing) { throw 'Existing profile overwritten' }
    $fallbackProfile = Join-Path $root 'fallback'
    & $prepare -SystemDataPath $fallbackProfile -WorkspacePath (Join-Path $root 'absent') -ReferencePath $source
    if ((Get-FileHash -LiteralPath (Join-Path $fallbackProfile $relative)).Hash -ne $expected) { throw 'Reference fallback failed' }
    $isolatedProfile = Join-Path $root 'isolated-no-workspace'
    $rejectedWorkspace = $false
    try { & $prepare -SystemDataPath $isolatedProfile -WorkspacePath $workspace -ReferencePath $missing -NoWorkspaceFallback } catch { $rejectedWorkspace = $true }
    if (-not $rejectedWorkspace -or (Test-Path -LiteralPath $isolatedProfile)) { throw 'Isolated mode inherited workspace cache or wrote on failure' }
    & $prepare -SystemDataPath $isolatedProfile -WorkspacePath $workspace -ReferencePath $source -NoWorkspaceFallback
    if ((Get-FileHash -LiteralPath (Join-Path $isolatedProfile $relative)).Hash -ne $expected) { throw 'Isolated explicit reference failed' }
    $failed = $false
    try { & $prepare -SystemDataPath (Join-Path $root 'unseeded') -WorkspacePath (Join-Path $root 'absent') -ReferencePath $missing } catch { $failed = $true }
    if (-not $failed) { throw 'Missing prerequisite silently accepted' }
    Write-Output 'PASS: cache preservation, explicit reference, isolated workspace rejection, missing prerequisite'
} finally {
    if (Test-Path -LiteralPath $root) { Remove-Item -LiteralPath $root -Recurse -Force }
}

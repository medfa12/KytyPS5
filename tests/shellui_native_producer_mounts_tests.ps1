$ErrorActionPreference = 'Stop'
. "$PSScriptRoot/../tools/shellui_native_producer_mounts.ps1"
$root = Join-Path $PSScriptRoot "../_TempData/native-producer-$([guid]::NewGuid())"
function Assert($condition, [string]$message) { if (-not $condition) { throw $message } }
try {
    $fw = Join-Path $root 'firmware with spaces'
    $stage = Join-Path $root 'copied stage'
    New-Item -ItemType Directory -Path "$fw/system_b/priv/lib", $stage -Force | Out-Null
    $source = Join-Path $fw 'system_b/priv/lib/libSceComposite.sprx'
    $copy = Join-Path $stage 'libSceComposite.sprx'
    [IO.File]::WriteAllBytes($source, [byte[]](1, 2, 3, 4))
    Copy-Item -LiteralPath $source -Destination $copy
    $mounts = @(Get-ShellUiNativeProducerMounts -FirmwareRoot $fw -CommonStage $stage)
    Assert ($mounts.Count -eq 6) 'all native/managed aliases must be explicit file mounts'
    $prefixes = @('/common_ex/lib', '/kyty/common_ex/lib', '/system_ex/common_ex/lib')
    for ($i = 0; $i -lt 3; $i++) {
        Assert ($mounts[2 * $i] -eq '--mount') 'mount argument missing'
        Assert ($mounts[2 * $i + 1] -eq "$($prefixes[$i])/libSceComposite.sprx=$([IO.Path]::GetFullPath($source))") 'alias does not select canonical producer'
    }
    [IO.File]::WriteAllBytes($copy, [byte[]](4, 3, 2, 1))
    $rejected = $false
    try { Get-ShellUiNativeProducerMounts -FirmwareRoot $fw -CommonStage $stage | Out-Null } catch { $rejected = $true }
    Assert $rejected 'different staged library was silently replaced'
    Assert ((Get-FileHash $source).Hash -ne (Get-FileHash $copy).Hash) 'validation mutated an input'
    Remove-Item -LiteralPath $copy
    Assert (@(Get-ShellUiNativeProducerMounts -FirmwareRoot $fw -CommonStage $stage).Count -eq 6) 'canonical producer must work without a staged duplicate'
    Assert (-not (Test-Path -LiteralPath $copy)) 'mount generation created a staged file'
    Remove-Item -LiteralPath $source
    $rejected = $false
    try { Get-ShellUiNativeProducerMounts -FirmwareRoot $fw -CommonStage $stage | Out-Null } catch { $rejected = $true }
    Assert $rejected 'missing producer accepted'
    'shellui_native_producer_mounts: passed'
} finally {
    Remove-Item -LiteralPath $root -Recurse -Force -ErrorAction SilentlyContinue
}

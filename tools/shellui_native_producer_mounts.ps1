function Get-ShellUiNativeProducerMounts {
    param(
        [Parameter(Mandatory = $true)][string]$FirmwareRoot,
        [Parameter(Mandatory = $true)][string]$CommonStage
    )

    # PSM initializes Composite from /system/priv/lib. SystemMonitor's P/Invoke
    # searches common_ex/lib. A copied stage file otherwise gets a second native
    # singleton: module_start does not initialize its compositor client. Route
    # both guest spellings to the same host file, preserving loader identity and
    # its once-only lifecycle instead of merging arbitrary equal-byte modules.
    $name = 'libSceComposite.sprx'
    $source = [System.IO.Path]::GetFullPath((Join-Path $FirmwareRoot "system_b/priv/lib/$name"))
    $staged = Join-Path $CommonStage $name
    if (-not (Test-Path -LiteralPath $source -PathType Leaf)) {
        throw "Missing native compositor producer: $source"
    }
    if (Test-Path -LiteralPath $staged) {
        if (-not (Test-Path -LiteralPath $staged -PathType Leaf) -or
            (Get-FileHash -LiteralPath $source -Algorithm SHA256).Hash -ne
            (Get-FileHash -LiteralPath $staged -Algorithm SHA256).Hash) {
            throw "Staged compositor differs from selected firmware: $staged"
        }
    }
    foreach ($prefix in @('/common_ex/lib', '/kyty/common_ex/lib', '/system_ex/common_ex/lib')) {
        '--mount'
        "$prefix/$name=$source"
    }
}

param(
    [string]$LogA = 'chainSUIparA',
    [string]$LogB = 'chainSUIparB',
    [ValidateRange(0, 86400)][int]$Seconds = 240,
    [ValidateRange(0, 86400)][int]$IdleSeconds = 45,
    [switch]$VisibleA,
    [switch]$OfflineDatabases,
    [switch]$DevelopmentApps
)
# Parallel ShellUI boots: two emulator processes, separate working dirs
# (own _kyty.txt each), separate frame dirs, shared binary + firmware.
# Usage inspects _Runs/<LogName>/_kyty.txt per run after both finish.
$workspace = Split-Path -Parent $PSScriptRoot
$frameEvery = $env:KYTY_FRAME_DUMP_EVERY
if ([string]::IsNullOrEmpty($frameEvery)) { $frameEvery = '20' }
$idleMs = $env:KYTY_IDLE_PRESENT_MS
if ([string]::IsNullOrEmpty($idleMs)) { $idleMs = '4000' }

$jobs = @()
foreach ($cfg in @(@{ Log = $LogA; Vis = $VisibleA }, @{ Log = $LogB; Vis = $false })) {
    $workdir = "$workspace/_Runs/$($cfg.Log)"
    New-Item -ItemType Directory -Path $workdir -Force | Out-Null
    $jobs += Start-Job -ScriptBlock {
        param($ws, $log, $sec, $idle, $vis, $frames, $idleMs, $dumpEvery, $db, $dev)
        $env:KYTY_FRAME_DUMP_DIR = $frames
        $env:KYTY_FLUSH_LOG = '1'
        $env:KYTY_IDLE_PRESENT_MS = $idleMs
        $env:KYTY_FRAME_DUMP_EVERY = $dumpEvery
        $visBool = [bool]::Parse($vis.ToString())
        $cmd = "& `"$ws/tools/run_shellui_1360.ps1`" -LogName $log -Seconds $sec " +
            "-IdleSeconds $idle -WorkDir `"$ws/_Runs/$log`""
        if ($visBool) { $cmd += ' -Visible' }
        if ($db) { $cmd += ' -OfflineDatabases' }
        if ($dev) { $cmd += ' -DevelopmentApps' }
        Invoke-Expression $cmd
    } -ArgumentList $workspace, $cfg.Log, $Seconds, $IdleSeconds, $cfg.Vis,
        "$workspace/_TempData/frames_$($cfg.Log)", $idleMs, $frameEvery,
        $OfflineDatabases.IsPresent, $DevelopmentApps.IsPresent
}
Write-Output "Launched $($jobs.Count) parallel boots; waiting up to $($Seconds + 60)s"
$jobs | Wait-Job -Timeout ($Seconds + 120) | Out-Null
foreach ($j in $jobs) {
    Write-Output "=== job $($j.Id) state=$($j.State) ==="
    Receive-Job $j | Select-Object -Last 6
}
$jobs | Remove-Job -Force

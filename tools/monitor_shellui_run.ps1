#requires -Version 5.1
<#
.SYNOPSIS
    Read-only run monitor for the PS5 ShellUI/RNPS harness.

.DESCRIPTION
    Watches one run (by name) and prints a verdict the moment it can be
    determined.  It never starts, stops or writes to the emulator; the only
    writes are crash artefacts under _Runs/<name>/crash/ after the emulator
    has already died with a non-clean exit.

    Two modes:
      -LogName <name>   attach to a live (or just-finished) run
      -SelfTest         exercise the classifier against synthetic streams

    Exit codes: 0 ok/guest-exit/user-close, 2 crash, 3 stall/hang/gone/no-log,
                4 self-test failure, 1 usage error.

.NOTES
    Windows PowerShell 5.1.  The hot guest stream (_kyty.txt, ~150 MB) is read
    through one persistent FileStream opened at its current length; only new
    bytes are decoded.  No Get-Content calls touch it in the polling loop.
#>
[CmdletBinding(DefaultParameterSetName = 'Watch')]
param(
    [Parameter(ParameterSetName = 'Watch', Position = 0)]
    [string]$LogName = '',

    [Parameter(ParameterSetName = 'SelfTest', Mandatory = $true)]
    [switch]$SelfTest,

    # Workspace that contains _Runs/ and _Logs/.  Overridable for copied runs.
    [string]$Root = (Split-Path -Parent $PSScriptRoot),

    # Hard wall limit for the watcher itself. 0 = no limit.  Reaching it does
    # NOT stop the emulator; it only prints a final verdict and exits.
    [ValidateRange(0, 86400)][int]$Seconds = 0,

    # Conclude stall/hang after this many seconds without a *meaningful* line.
    [ValidateRange(1, 86400)][int]$QuietAfter = 120,

    # Keep watching after a verdict.  Useful when attaching to a run you did
    # not start, or when the launcher may relaunch after a stall.
    [switch]$Follow,

    # Extra idle-narration regex, appended to the built-in ignore list.
    [string]$IgnorePattern = '',

    [ValidateRange(50, 60000)][int]$PollMs = 1000,

    # How many meaningful lines to retain for diagnostics (bounded memory).
    [ValidateRange(10, 5000)][int]$RecentLimit = 200,

    # A process whose TotalProcessorTime has not moved for this long is
    # considered not advancing (HANG candidate, subject to QuietAfter).
    [ValidateRange(2, 600)][int]$CpuFlatSeconds = 10,

    # Do not use _Logs/<name>.result.json as an exit-code fallback.  The
    # launcher writes that file after its own monitor finishes.
    [switch]$NoResultJson
)

Set-StrictMode -Version 2.0
$ErrorActionPreference = 'Stop'

# ---------------------------------------------------------------------------
# Constants / tunables
# ---------------------------------------------------------------------------

$script:StreamKeys = @('kyty', 'stdout', 'stderr')
$script:ChunkBytes = 1MB          # max bytes read per stream per poll
$script:LagKeepBytes = 4MB        # after a backlog skip, keep this much tail
$script:MaxLagBytes = 16MB        # never let a 150 MB burst build a backlog
$script:PendingLimit = 512KB      # unterminated line memory guard
$script:NoLogGrace = 5            # seconds before "no-log" on a live attach
$script:NoPidGrace = 5            # seconds before "gone" with no pid
$script:FollowGrace = 15          # seconds to wait for a relaunch under -Follow
$script:RecentLimit = $RecentLimit
$script:CpuFlatSeconds = $CpuFlatSeconds

# Idle narration written while every guest thread is parked.  These lines must
# never count as progress; otherwise a frozen boot looks perfectly healthy.
# Keep this list aligned with the known narration families documented in
# docs/emulator-monitor.md.
$script:IgnoreDefault = New-Object System.Text.RegularExpressions.Regex(
    '(Equeue wait|flipPendingNum|PthreadCondWait|vm\.stats|CP packet|setspecific|regs\s*=|Mono: mono_w32handle|\[SceSoCPowerWatch\])',
    [System.Text.RegularExpressions.RegexOptions]::IgnoreCase)

$script:IgnoreUser = $null
if ($IgnorePattern -ne '') {
    try {
        $script:IgnoreUser = New-Object System.Text.RegularExpressions.Regex(
            $IgnorePattern, [System.Text.RegularExpressions.RegexOptions]::IgnoreCase)
    } catch {
        [Console]::Error.WriteLine("monitor: invalid -IgnorePattern: $($_.Exception.Message)")
        exit 1
    }
}

# Ordered progress milestones.  Order is the contract: a lower stage never
# overwrites a higher one, so late idle chatter cannot "rewind" progress.
$script:Milestones = @(
    (New-Object psobject -Property @{ Name = 'PUI init';                  Regex = (New-Object System.Text.RegularExpressions.Regex('PUI\s*init', 'IgnoreCase')) }),
    (New-Object psobject -Property @{ Name = 'scene load';                Regex = (New-Object System.Text.RegularExpressions.Regex('scene[ ._-]*load', 'IgnoreCase')) }),
    (New-Object psobject -Property @{ Name = '_sceRnpsBundleRegistryInit'; Regex = (New-Object System.Text.RegularExpressions.Regex('_sceRnpsBundleRegistryInit', 'IgnoreCase')) }),
    (New-Object psobject -Property @{ Name = 'PopulatePool';              Regex = (New-Object System.Text.RegularExpressions.Regex('PopulatePool', 'IgnoreCase')) }),
    (New-Object psobject -Property @{ Name = 'TTPoolInstance';            Regex = (New-Object System.Text.RegularExpressions.Regex('TTPoolInstance', 'IgnoreCase')) }),
    (New-Object psobject -Property @{ Name = 'JSExecutorPreloadHostDll';  Regex = (New-Object System.Text.RegularExpressions.Regex('JSExecutorPreloadHostDll', 'IgnoreCase')) }),
    (New-Object psobject -Property @{ Name = 'Evaluating bundle';         Regex = (New-Object System.Text.RegularExpressions.Regex('Evaluating\s+bundle', 'IgnoreCase')) }),
    (New-Object psobject -Property @{ Name = 'EventPluginRegistry';       Regex = (New-Object System.Text.RegularExpressions.Regex('EventPluginRegistry', 'IgnoreCase')) }),
    (New-Object psobject -Property @{ Name = 'canvas submit/present';     Regex = (New-Object System.Text.RegularExpressions.Regex('(canvas.*(submit|present))|(frame[ ._-]?(draw|present|submit))|OnLaunched|frame-dump|^\s*(present|submit)\b', 'IgnoreCase')) })
)
$script:MilestoneQuick = New-Object System.Text.RegularExpressions.Regex(
    '(PUI\s*init|scene[ ._-]*load|_sceRnpsBundleRegistryInit|PopulatePool|TTPoolInstance|JSExecutorPreloadHostDll|Evaluating\s+bundle|EventPluginRegistry|canvas.*(submit|present)|frame[ ._-]?(draw|present|submit)|OnLaunched|frame-dump)',
    [System.Text.RegularExpressions.RegexOptions]::IgnoreCase)

# User closed the shell window.  These markers suppress crash classification:
# the host exit code is then an artefact of shutdown, not a fault.
$script:UserCloseRegex = New-Object System.Text.RegularExpressions.Regex(
    '(Window\s+\d+\s+closed|Event:\s*quit|user\s*close|window\s+close)',
    [System.Text.RegularExpressions.RegexOptions]::IgnoreCase)

# Annotation only (never classification): a text-level hint that the host
# faulted.  The verdict is still driven by the OS exit code.
$script:CrashMarkerRegex = New-Object System.Text.RegularExpressions.Regex(
    '(Unhandled host exception|STACK_OVERFLOW|ACCESS_VIOLATION|Faulting host module|Exception code)',
    [System.Text.RegularExpressions.RegexOptions]::IgnoreCase)

$script:MilestoneJustAdvanced = $null

# ---------------------------------------------------------------------------
# Native process handle: lets an attached watcher read the exit code of a
# process it did not start.  Get-Process cannot do that after the exit.
# ---------------------------------------------------------------------------

if (-not ('KytyProcNative' -as [type])) {
    Add-Type -TypeDefinition @"
using System;
using System.Runtime.InteropServices;
public static class KytyProcNative {
    [DllImport("kernel32.dll", SetLastError=true)]
    public static extern IntPtr OpenProcess(uint dwDesiredAccess, bool bInheritHandle, int dwProcessId);
    [DllImport("kernel32.dll", SetLastError=true)]
    public static extern bool GetExitCodeProcess(IntPtr hProcess, out uint lpExitCode);
    [DllImport("kernel32.dll", SetLastError=true)]
    public static extern bool CloseHandle(IntPtr hObject);
    [DllImport("kernel32.dll", SetLastError=true)]
    public static extern uint WaitForSingleObject(IntPtr hHandle, uint dwMilliseconds);
}
"@
}

# File identity (volume serial + file index) is the only reliable way to see
# that a rotated path now names a different file.  CreationTimeUtc is not
# enough: NTFS timestamps have coarse granularity, and FileMode.Create keeps
# the creation time of an overwritten file.
if (-not ('KytyFileNative' -as [type])) {
    Add-Type -TypeDefinition @"
using System;
using System.Runtime.InteropServices;
public static class KytyFileNative {
    [StructLayout(LayoutKind.Sequential)]
    public struct BY_HANDLE_FILE_INFORMATION {
        public uint FileAttributes;
        public System.Runtime.InteropServices.ComTypes.FILETIME CreationTime;
        public System.Runtime.InteropServices.ComTypes.FILETIME LastAccessTime;
        public System.Runtime.InteropServices.ComTypes.FILETIME LastWriteTime;
        public uint VolumeSerialNumber;
        public uint FileSizeHigh;
        public uint FileSizeLow;
        public uint NumberOfLinks;
        public uint FileIndexHigh;
        public uint FileIndexLow;
    }
    [DllImport("kernel32.dll", SetLastError=true)]
    public static extern bool GetFileInformationByHandle(IntPtr hFile, out BY_HANDLE_FILE_INFORMATION info);
}
"@
}

# ---------------------------------------------------------------------------
# Small formatting helpers (invariant culture so verdicts are machine-readable)
# ---------------------------------------------------------------------------

function Format-Number {
    param([double]$Value, [string]$Format = '0.0')
    return $Value.ToString($Format, [System.Globalization.CultureInfo]::InvariantCulture)
}

function Shorten-Line {
    param([string]$Text, [int]$Max = 160)
    if ($null -eq $Text) { return '' }
    $t = $Text.Trim()
    $t = [System.Text.RegularExpressions.Regex]::Replace($t, '\s+', ' ')
    if ($t.Length -le $Max) { return $t }
    if ($Max -le 3) { return $t.Substring(0, $Max) }
    return $t.Substring(0, $Max - 3) + '...'
}

function Format-ExitHex {
    param([int64]$Code)
    $u = [uint32]($Code -band 0xFFFFFFFFL)
    return ('0x{0:X8}' -f $u)
}

function Convert-ExitCode {
    param([uint32]$Code)
    if ($Code -ge 2147483648) { return ([int64]$Code - 4294967296) }
    return [int64]$Code
}

function Get-CrashKind {
    param([int64]$Code)
    switch ($Code) {
        -1073741819 { return 'access-violation(0xC0000005)' }
        -1073741571 { return 'stack-overflow(0xC00000FD)' }
        -1073740791 { return 'fail-fast(0xC0000409)' }
        -1073740940 { return 'heap-corruption(0xC0000374)' }
        -1          { return 'terminated-externally(0xFFFFFFFF)' }
        default     { return 'non-zero-exit' }
    }
}

function Emit {
    param([string]$Line)
    # Console output, not Write-Output: callers return an exit code from the
    # same function, and pipeline output would be swallowed into that return
    # value.  It also keeps a launcher's redirected log timely.
    try {
        [Console]::Out.WriteLine($Line)
        [Console]::Out.Flush()
    } catch { }
}

function Close-ProcHandle {
    if ($null -eq $script:Proc) { return }
    if ($script:Proc.Handle -ne [IntPtr]::Zero) {
        [void][KytyProcNative]::CloseHandle($script:Proc.Handle)
        $script:Proc.Handle = [IntPtr]::Zero
    }
}

function Open-ProcHandle {
    if ($null -eq $script:Proc -or $null -eq $script:Proc.Pid) { return }
    if ($script:Proc.Handle -ne [IntPtr]::Zero) { return }
    # PROCESS_QUERY_LIMITED_INFORMATION (0x1000) | SYNCHRONIZE (0x100000)
    try {
        $h = [KytyProcNative]::OpenProcess(0x101000, $false, [int]$script:Proc.Pid)
        if ($h -ne [IntPtr]::Zero) { $script:Proc.Handle = $h }
    } catch { }
}

function Read-ExitCodeFromHandle {
    if ($null -eq $script:Proc -or $script:Proc.Handle -eq [IntPtr]::Zero) { return $null }
    try {
        $wait = [KytyProcNative]::WaitForSingleObject($script:Proc.Handle, 0)
        if ($wait -ne 0) { return $null }  # still running (or wait failed)
        $code = [uint32]0
        if (-not [KytyProcNative]::GetExitCodeProcess($script:Proc.Handle, [ref]$code)) { return $null }
        return (Convert-ExitCode $code)
    } catch {
        return $null
    }
}

# ---------------------------------------------------------------------------
# Analysis state
# ---------------------------------------------------------------------------

function New-AnalysisState {
    param([DateTime]$At)
    $q = New-Object 'System.Collections.Generic.Queue[string]'
    return (New-Object psobject -Property @{
        RawLines              = [int64]0
        IgnoredLines          = [int64]0
        MeaningfulLines       = [int64]0
        RawAtLastMeaningful   = [int64]0
        MilestoneIndex        = -1
        MilestoneName         = '(none)'
        MilestoneHits         = [int64]0
        LastMeaningful        = '(none)'
        LastMeaningfulAt      = $At
        UserClose             = $false
        GuestExitMarker       = $false
        Recent                = $q
        Restarts              = [int64]0
        SkippedBytes          = [int64]0
    })
}

function Reset-Analysis {
    param($Analysis, [DateTime]$At)
    $Analysis.RawLines = [int64]0
    $Analysis.IgnoredLines = [int64]0
    $Analysis.MeaningfulLines = [int64]0
    $Analysis.RawAtLastMeaningful = [int64]0
    $Analysis.MilestoneIndex = -1
    $Analysis.MilestoneName = '(none)'
    $Analysis.MilestoneHits = [int64]0
    $Analysis.LastMeaningful = '(none)'
    $Analysis.LastMeaningfulAt = $At
    $Analysis.UserClose = $false
    $Analysis.GuestExitMarker = $false
    $Analysis.Recent.Clear()
}

# ---------------------------------------------------------------------------
# Tail reader for crash artefacts (bounded; never reads a whole 150 MB file)
# ---------------------------------------------------------------------------

function Get-FileTailLines {
    param(
        [string]$Path,
        [int]$Count = 400,
        [int]$MaxBytes = 4MB
    )
    if ($Count -le 0) { return @() }
    if (-not (Test-Path -LiteralPath $Path)) { return @() }

    $fs = $null
    try {
        $fs = [System.IO.File]::Open($Path,
            [System.IO.FileMode]::Open,
            [System.IO.FileAccess]::Read,
            ([System.IO.FileShare]::ReadWrite -bor [System.IO.FileShare]::Delete))
        $len = $fs.Length
        if ($len -le 0) { return @() }

        $take = [int][Math]::Min([int64]$len, [int64]$MaxBytes)
        [void]$fs.Seek($len - $take, [System.IO.SeekOrigin]::Begin)
        $buf = New-Object byte[] $take
        $read = 0
        while ($read -lt $take) {
            $n = $fs.Read($buf, $read, $take - $read)
            if ($n -le 0) { break }
            $read += $n
        }
        if ($read -le 0) { return @() }

        $text = [System.Text.Encoding]::UTF8.GetString($buf, 0, $read)
        if ($take -lt $len) {
            # Started mid-line.  Drop that partial first line.
            $nl = $text.IndexOf([char]10)
            if ($nl -ge 0) { $text = $text.Substring($nl + 1) } else { $text = '' }
        }

        $raw = $text -split "`n"
        $list = New-Object 'System.Collections.Generic.List[string]'
        foreach ($l in $raw) {
            $s = $l
            if ($s.Length -gt 0 -and $s[$s.Length - 1] -eq "`r") {
                $s = $s.Substring(0, $s.Length - 1)
            }
            [void]$list.Add($s)
        }
        # A trailing newline produces one empty element; it is not a line.
        if ($list.Count -gt 0 -and $list[$list.Count - 1] -eq '') {
            $list.RemoveAt($list.Count - 1)
        }
        if ($list.Count -le $Count) { return $list.ToArray() }

        $out = New-Object 'System.Collections.Generic.List[string]'
        for ($i = $list.Count - $Count; $i -lt $list.Count; $i++) {
            [void]$out.Add($list[$i])
        }
        return $out.ToArray()
    } catch {
        return @("(tail read failed: $($_.Exception.Message))")
    } finally {
        if ($null -ne $fs) { $fs.Dispose() }
    }
}

function Write-Utf8Text {
    param([string]$Path, [string]$Text)
    $enc = New-Object System.Text.UTF8Encoding($false)
    [System.IO.File]::WriteAllText($Path, $Text, $enc)
}

function Write-Utf8Lines {
    param([string]$Path, [string[]]$Lines)
    $enc = New-Object System.Text.UTF8Encoding($false)
    if ($null -eq $Lines) { $Lines = @() }
    [System.IO.File]::WriteAllLines($Path, $Lines, $enc)
}

function Write-CrashArtifacts {
    param(
        $Context,
        [string]$Dir,
        [string]$KytyPath,
        [string]$StdoutPath,
        [string]$StderrPath,
        [string]$LogName
    )
    if ($null -eq $Context.ExitCode) { return $null }
    [void](New-Item -ItemType Directory -Path $Dir -Force)

    $exitHex = Format-ExitHex -Code ([int64]$Context.ExitCode)
    $verdict = @(
        'class: crash',
        ("exit: {0} ({1})" -f $exitHex, $Context.ExitCode),
        ("kind: {0}" -f $Context.Kind),
        ("exitSource: {0}" -f $Context.ExitSource),
        ("wallSeconds: {0}" -f (Format-Number -Value $Context.Wall)),
        ("lastMilestone: {0}" -f $Context.LastMilestone),
        ("secondsSinceLastMeaningful: {0}" -f (Format-Number -Value $Context.Silence)),
        ("lastMeaningful: {0}" -f (Shorten-Line -Text $Context.LastMeaningful -Max 600)),
        ("meaningfulLines: {0}" -f $Context.MeaningfulLines),
        ("rawLines: {0}" -f $Context.RawLines),
        ("userCloseMarkers: {0}" -f $Context.UserClose),
        ("pid: {0}" -f $Context.Pid),
        ("attempt: {0}" -f $Context.Attempt),
        ("logName: {0}" -f $LogName),
        ("logCrashMarker: {0}" -f $Context.LogCrashMarker),
        ("writtenUtc: {0}" -f ([DateTime]::UtcNow.ToString('o')))
    )
    Write-Utf8Lines -Path (Join-Path $Dir 'verdict.txt') -Lines $verdict

    $kytyTail = @(Get-FileTailLines -Path $KytyPath -Count 400 -MaxBytes (4MB))
    if ($kytyTail.Count -eq 0) { $kytyTail = @('(missing or empty)') }
    Write-Utf8Lines -Path (Join-Path $Dir 'kyty_tail.txt') -Lines $kytyTail

    # Whole stderr when small (the interesting stack / loader message is
    # usually the first line), otherwise the tail.
    $stderrText = $null
    if (Test-Path -LiteralPath $StderrPath) {
        try {
            $sLen = (Get-Item -LiteralPath $StderrPath -ErrorAction Stop).Length
            if ($sLen -lt 65536) {
                $stderrText = [System.IO.File]::ReadAllText($StderrPath)
            } else {
                $stderrText = (@(Get-FileTailLines -Path $StderrPath -Count 400 -MaxBytes (4MB)) -join "`r`n")
            }
        } catch {
            $stderrText = "(stderr read failed: $($_.Exception.Message))"
        }
    } else {
        $stderrText = '(missing)'
    }
    Write-Utf8Text -Path (Join-Path $Dir 'stderr_tail.txt') -Text $stderrText

    $stdoutTail = @(Get-FileTailLines -Path $StdoutPath -Count 200 -MaxBytes (1MB))
    if ($stdoutTail.Count -eq 0) { $stdoutTail = @('(missing or empty)') }
    Write-Utf8Lines -Path (Join-Path $Dir 'stdout_tail.txt') -Lines $stdoutTail

    return $Dir
}

# ---------------------------------------------------------------------------
# Incremental stream readers
# ---------------------------------------------------------------------------

function Get-StreamFileIdentity {
    param($Stream)
    if ($null -eq $Stream) { return $null }
    try {
        $info = New-Object KytyFileNative+BY_HANDLE_FILE_INFORMATION
        $h = $Stream.SafeFileHandle.DangerousGetHandle()
        if ([KytyFileNative]::GetFileInformationByHandle($h, [ref]$info)) {
            return ('{0:X8}:{1:X8}{2:X8}' -f $info.VolumeSerialNumber, $info.FileIndexHigh, $info.FileIndexLow)
        }
    } catch { }
    return $null
}

function Get-PathFileIdentity {
    param([string]$Path)
    $fs = $null
    try {
        $fs = [System.IO.File]::Open($Path,
            [System.IO.FileMode]::Open,
            [System.IO.FileAccess]::Read,
            ([System.IO.FileShare]::ReadWrite -bor [System.IO.FileShare]::Delete))
        return (Get-StreamFileIdentity -Stream $fs)
    } catch {
        return $null
    } finally {
        if ($null -ne $fs) { $fs.Dispose() }
    }
}

function Open-ReaderStream {
    param($Reader, [switch]$FromStart)
    $fs = [System.IO.File]::Open($Reader.Path,
        [System.IO.FileMode]::Open,
        [System.IO.FileAccess]::Read,
        ([System.IO.FileShare]::ReadWrite -bor [System.IO.FileShare]::Delete))
    if (-not $FromStart) { [void]$fs.Seek(0, [System.IO.SeekOrigin]::End) }
    $Reader.Stream = $fs
    $Reader.Decoder = [System.Text.Encoding]::UTF8.GetDecoder()
    $Reader.Pending = ''
    $Reader.DropPartial = $false
    $Reader.LastLength = $fs.Length
    $fi = Get-Item -LiteralPath $Reader.Path -ErrorAction Stop
    $Reader.CreationUtc = $fi.CreationTimeUtc
    $Reader.FileId = Get-StreamFileIdentity -Stream $fs
}

function New-ReaderState {
    param([string]$Path, [switch]$FromStart)
    $reader = New-Object psobject -Property @{
        Path        = $Path
        Stream      = $null
        Decoder     = $null
        Pending     = ''
        DropPartial = $false
        CreationUtc = [DateTime]::MinValue
        FileId      = $null
        LastLength  = [int64]0
        Restarts    = [int64]0
        Skipped     = [int64]0
        BytesRead   = [int64]0
    }
    Open-ReaderStream -Reader $reader -FromStart:$FromStart
    return $reader
}

function Close-Reader {
    param($Reader)
    if ($null -eq $Reader) { return }
    if ($null -ne $Reader.Stream) {
        try { $Reader.Stream.Dispose() } catch { }
        $Reader.Stream = $null
    }
}

# Folds one logical line into the analysis.  Shared by the incremental
# reader and the terminal pending-line flush so both apply identical rules.
function Add-AnalysisLine {
    param($Analysis, [string]$Line, [DateTime]$Now)
    $Analysis.RawLines++
    if ($Line.Length -eq 0) { return }
    $trim = $Line.Trim()
    if ($trim.Length -eq 0) { return }

    if ($script:IgnoreDefault.IsMatch($Line) -or
        ($null -ne $script:IgnoreUser -and $script:IgnoreUser.IsMatch($Line))) {
        $Analysis.IgnoredLines++
        return
    }

    $Analysis.MeaningfulLines++
    $Analysis.LastMeaningful = $trim
    $Analysis.LastMeaningfulAt = $Now
    $Analysis.RawAtLastMeaningful = $Analysis.RawLines
    while ($Analysis.Recent.Count -ge $script:RecentLimit) {
        [void]$Analysis.Recent.Dequeue()
    }
    [void]$Analysis.Recent.Enqueue($trim)

    if ($script:UserCloseRegex.IsMatch($Line)) { $Analysis.UserClose = $true }
    if ($script:CrashMarkerRegex.IsMatch($Line)) { $Analysis.GuestExitMarker = $true }

    if ($script:MilestoneQuick.IsMatch($Line)) {
        $hit = -1
        for ($m = 0; $m -lt $script:Milestones.Count; $m++) {
            if ($script:Milestones[$m].Regex.IsMatch($Line)) { $hit = $m }
        }
        if ($hit -ge 0) {
            $Analysis.MilestoneHits++
            if ($hit -gt $Analysis.MilestoneIndex) {
                $Analysis.MilestoneIndex = $hit
                $Analysis.MilestoneName = $script:Milestones[$hit].Name
                $script:MilestoneJustAdvanced = $Analysis.MilestoneName
            }
        }
    }
}

# Emits a trailing line that never got its newline.  Only call this once the
# stream is terminal (process dead or caller concluding): a live partial line
# may still be completed by the next poll, and flushing early would duplicate
# it.  Returns $true when something was emitted.
function Flush-ReaderPending {
    param($Reader, $Analysis, [DateTime]$Now)
    if ($null -eq $Reader -or [string]::IsNullOrEmpty($Reader.Pending)) { return $false }
    $line = $Reader.Pending
    $Reader.Pending = ''
    if ($line.Length -gt 0 -and $line[$line.Length - 1] -eq "`r") {
        $line = $line.Substring(0, $line.Length - 1)
    }
    Add-AnalysisLine -Analysis $Analysis -Line $line -Now $Now
    return $true
}

# Reads at most MaxBytes of new data and folds every complete line into
# $Analysis.  Returns a small result object.  On truncation/rotation the
# reader is reopened at byte 0; with -ResetOnRestart the analysis is reset
# first so milestone/silence state belongs to the new attempt.
function Read-ReaderIntoAnalysis {
    param(
        $Reader,
        $Analysis,
        [DateTime]$Now,
        [bool]$ResetOnRestart = $false,
        [int]$MaxBytes = $script:ChunkBytes,
        [int64]$MaxLagBytes = $script:MaxLagBytes,
        [int64]$LagKeepBytes = $script:LagKeepBytes
    )
    $res = New-Object psobject -Property @{
        Missing   = $false
        Restarted = $false
        Bytes     = [int64]0
        Lines     = [int64]0
        Skipped   = [int64]0
        Error     = $null
    }
    if ($null -eq $Reader -or $null -eq $Reader.Stream) { $res.Missing = $true; return $res }

    try {
        $fi = Get-Item -LiteralPath $Reader.Path -ErrorAction SilentlyContinue
        if ($null -eq $fi) {
            Close-Reader -Reader $Reader
            $res.Missing = $true
            return $res
        }

        # Rotation: the path now names a different file.  Truncation: the file
        # became shorter than what we had already consumed.  Both mean the
        # emulator restarted its stream; resync from byte 0.
        $restart = $false
        if ($fi.Length -lt $Reader.LastLength) {
            $restart = $true
        } elseif ($Reader.Stream.Position -gt $fi.Length) {
            $restart = $true
        } else {
            # Let the OS tell us whether the path now names a different file.
            $pathId = Get-PathFileIdentity -Path $Reader.Path
            if ($null -ne $pathId -and $null -ne $Reader.FileId -and $pathId -ne $Reader.FileId) {
                $restart = $true
            } elseif ($fi.CreationTimeUtc -ne $Reader.CreationUtc) {
                $restart = $true
            }
        }

        if ($restart) {
            Close-Reader -Reader $Reader
            Open-ReaderStream -Reader $Reader -FromStart
            $Reader.Restarts++
            $res.Restarted = $true
            if ($ResetOnRestart) {
                Reset-Analysis -Analysis $Analysis -At $Now
                $Analysis.Restarts++
            }
        }

        $available = $Reader.Stream.Length - $Reader.Stream.Position
        if ($available -le 0) { $Reader.LastLength = $Reader.Stream.Length; return $res }

        # Do not chase an unbounded backlog: jump near the tail, drop the
        # partial line, and report the skipped byte count.  This keeps the
        # monitor live even after a multi-hundred-MB burst.
        if ($available -gt $MaxLagBytes) {
            $skip = $available - $LagKeepBytes
            if ($skip -gt 0) {
                [void]$Reader.Stream.Seek($skip, [System.IO.SeekOrigin]::Current)
                $Reader.Pending = ''
                $Reader.Decoder.Reset()
                $Reader.DropPartial = $true
                $Reader.Skipped += $skip
                $res.Skipped = $skip
                $available = $LagKeepBytes
            }
        }

        $toRead = [int][Math]::Min([int64]$available, [int64]$MaxBytes)
        if ($toRead -le 0) { $Reader.LastLength = $Reader.Stream.Length; return $res }

        $buf = New-Object byte[] $toRead
        $read = $Reader.Stream.Read($buf, 0, $toRead)
        if ($read -le 0) { $Reader.LastLength = $Reader.Stream.Length; return $res }
        $res.Bytes = $read
        $Reader.BytesRead += $read

        $chars = New-Object char[] ([System.Text.Encoding]::UTF8.GetMaxCharCount($read))
        $cc = $Reader.Decoder.GetChars($buf, 0, $read, $chars, 0, $false)
        $text = $Reader.Pending + [System.String]::new($chars, 0, $cc)

        if ($Reader.DropPartial) {
            $nl = $text.IndexOf([char]10)
            if ($nl -lt 0) {
                # Entire chunk is still inside the skipped line.
                $Reader.Pending = ''
                $Reader.LastLength = $Reader.Stream.Length
                return $res
            }
            $text = $text.Substring($nl + 1)
            $Reader.DropPartial = $false
        }

        $parts = $text.Split([char]10)
        $complete = $parts.Length - 1
        for ($i = 0; $i -lt $complete; $i++) {
            $line = $parts[$i]
            if ($line.Length -gt 0 -and $line[$line.Length - 1] -eq "`r") {
                $line = $line.Substring(0, $line.Length - 1)
            }
            $res.Lines++
            Add-AnalysisLine -Analysis $Analysis -Line $line -Now $Now
        }

        $Reader.Pending = $parts[$complete]
        if ($Reader.Pending.Length -gt $script:PendingLimit) {
            # An unterminated giant line (a dump with no newline).  Bound
            # memory, but do not pretend it was a progress line.
            $Reader.Pending = ''
            $Analysis.IgnoredLines++
        }
        $Reader.LastLength = $Reader.Stream.Length
    } catch {
        $res.Error = $_.Exception.Message
    }
    return $res
}

# ---------------------------------------------------------------------------
# Verdict classifier (pure; used by the live loop and the self-test)
# ---------------------------------------------------------------------------

function Get-Verdict {
    param(
        [bool]$LogExists,
        [bool]$ProcessKnown,
        [bool]$ProcessAlive,
        $ExitCode,
        [bool]$CpuAdvancing,
        [double]$SilenceSeconds,
        [double]$QuietAfter,
        [bool]$SawUserClose,
        [int64]$MeaningfulLines,
        [string]$LastMilestone,
        [bool]$TimeLimitHit,
        [double]$SinceStartSeconds
    )

    if (-not $LogExists) {
        if ($SinceStartSeconds -ge $script:NoLogGrace) { return 'no-log' }
        return 'running'
    }

    if ($ProcessKnown -and -not $ProcessAlive) {
        if ($null -eq $ExitCode) { return 'gone' }
        $code = [int64]$ExitCode
        # A user close owns the outcome even if shutdown then faulted: the
        # crash rule explicitly excludes exits preceded by these markers.
        if ($SawUserClose) { return 'user-close' }
        if ($code -eq 321) { return 'guest-exit' }
        # Stop-Process/TerminateProcess leaves -1 behind.  That is an external
        # termination, not a host fault code; keep it out of `crash`.
        if ($code -eq -1) { return 'gone' }
        if ($code -eq 0) { return 'ok' }
        return 'crash'
    }

    if (-not $ProcessKnown) {
        if ($SinceStartSeconds -ge $script:NoPidGrace) { return 'gone' }
        return 'running'
    }

    # Alive.
    if ($SilenceSeconds -ge $QuietAfter) {
        if ($CpuAdvancing) { return 'stall' } else { return 'hang' }
    }
    if ($TimeLimitHit) {
        if ($MeaningfulLines -gt 0) { return 'ok' }
        return 'running'
    }
    if ($MeaningfulLines -gt 0) { return 'ok' }
    return 'running'
}

function Get-ClassExitCode {
    param([string]$Class)
    switch ($Class) {
        'crash'                                        { return 2 }
        'stall'                                        { return 3 }
        'hang'                                         { return 3 }
        'gone'                                         { return 3 }
        'no-log'                                       { return 3 }
        default                                        { return 0 }
    }
}

# ---------------------------------------------------------------------------
# Process liveness (OS-driven)
# ---------------------------------------------------------------------------

function Read-PidFile {
    param([string]$Path)
    if (-not (Test-Path -LiteralPath $Path)) { return $null }
    try {
        $txt = [System.IO.File]::ReadAllText($Path).Trim()
        $n = 0
        if ([int]::TryParse($txt, [ref]$n) -and $n -gt 0) { return $n }
    } catch { }
    return $null
}

function Read-ResultExitCode {
    # NB: the parameter cannot be named $Pid: that identifier is the
    # read-only automatic $PID variable and binding would throw.
    param([string]$Path, [int]$ProcessId)
    if ($NoResultJson) { return $null }
    if (-not (Test-Path -LiteralPath $Path)) { return $null }
    try {
        $j = Get-Content -LiteralPath $Path -Raw -ErrorAction Stop | ConvertFrom-Json
        if ($null -eq $j) { return $null }
        if ($null -eq $j.pid -or $null -eq $j.exitCode) { return $null }
        if ([int]$j.pid -ne $ProcessId) { return $null }
        return [int64]$j.exitCode
    } catch {
        return $null
    }
}

# Returns $true when the pid changed (new attempt or recycled pid).
function Update-ProcState {
    param([DateTime]$Now)
    $changed = $false

    $filePid = Read-PidFile -Path $script:PidPath
    if ($null -ne $filePid -and $filePid -ne $script:Proc.Pid) {
        if ($null -ne $script:Proc.Pid) { $changed = $true }
        Close-ProcHandle
        $script:Proc.Pid = [int]$filePid
        $script:Proc.Known = $true
        $script:Proc.Alive = $false
        $script:Proc.StartUtc = $null
        $script:Proc.ExitCode = $null
        $script:Proc.ExitSource = ''
        $script:Proc.DeadSince = $null
        $script:Proc.CpuLast = $null
        $script:Proc.CpuLastAdvanceUtc = $null
        $script:Proc.CpuAdvancing = $true
        $script:Proc.CpuFlatSeconds = 0.0
        Open-ProcHandle
    }

    if ($null -eq $script:Proc.Pid) {
        $script:Proc.Known = $false
        $script:Proc.Alive = $false
        return $changed
    }

    $p = Get-Process -Id $script:Proc.Pid -ErrorAction SilentlyContinue
    if ($null -eq $p) {
        $script:Proc.Alive = $false
        $script:Proc.CpuAdvancing = $false
        if ($null -eq $script:Proc.DeadSince) { $script:Proc.DeadSince = $Now }

        if ($null -eq $script:Proc.ExitCode) {
            $code = Read-ExitCodeFromHandle
            if ($null -ne $code) {
                $script:Proc.ExitCode = $code
                $script:Proc.ExitSource = 'process-handle'
            }
        }
        if ($null -eq $script:Proc.ExitCode) {
            $code = Read-ResultExitCode -Path $script:ResultPath -ProcessId ([int]$script:Proc.Pid)
            if ($null -ne $code) {
                $script:Proc.ExitCode = $code
                $script:Proc.ExitSource = 'result.json'
            }
        }
        return $changed
    }

    $script:Proc.Alive = $true
    $script:Proc.DeadSince = $null

    try {
        $st = $p.StartTime.ToUniversalTime()
        if ($null -eq $script:Proc.StartUtc) {
            $script:Proc.StartUtc = $st
        } elseif ([Math]::Abs(($st - $script:Proc.StartUtc).TotalSeconds) -gt 1.0) {
            # Same pid, different start time: the pid was recycled.  Treat it
            # as a new process rather than inheriting the old exit code/CPU.
            Close-ProcHandle
            $script:Proc.StartUtc = $st
            $script:Proc.ExitCode = $null
            $script:Proc.ExitSource = ''
            $script:Proc.CpuLast = $null
            $script:Proc.CpuLastAdvanceUtc = $Now
            $script:Proc.CpuAdvancing = $true
            Open-ProcHandle
            $changed = $true
        }
    } catch { }

    try {
        $cpu = $p.TotalProcessorTime.TotalSeconds
        if ($null -eq $script:Proc.CpuLast) {
            $script:Proc.CpuLast = $cpu
            $script:Proc.CpuLastAdvanceUtc = $Now
            $script:Proc.CpuAdvancing = $true
        } elseif ($cpu -gt ($script:Proc.CpuLast + 0.05)) {
            $script:Proc.CpuLast = $cpu
            $script:Proc.CpuLastAdvanceUtc = $Now
        }
        $flat = ($Now - $script:Proc.CpuLastAdvanceUtc).TotalSeconds
        if ($flat -lt 0) { $flat = 0.0 }
        $script:Proc.CpuFlatSeconds = $flat
        $script:Proc.CpuAdvancing = ($flat -lt $script:CpuFlatSeconds)
    } catch { }

    return $changed
}

# ---------------------------------------------------------------------------
# Live watcher
# ---------------------------------------------------------------------------

function Format-Metrics {
    param($Ctx)
    $parts = New-Object 'System.Collections.Generic.List[string]'
    if ($null -ne $Ctx.Pid) { [void]$parts.Add("pid=$($Ctx.Pid)") }
    [void]$parts.Add("wall=$(Format-Number $Ctx.Wall)s")
    [void]$parts.Add("silent=$(Format-Number $Ctx.Silence)s")
    if ($Ctx.ProcessAlive) {
        if ($Ctx.CpuAdvancing) { [void]$parts.Add('cpu=advancing') } else { [void]$parts.Add('cpu=flat') }
    } else {
        [void]$parts.Add('cpu=gone')
    }
    [void]$parts.Add("meaningful=$($Ctx.MeaningfulLines)")
    [void]$parts.Add("raw=$($Ctx.RawLines)")
    [void]$parts.Add("milestone=`"$($Ctx.LastMilestone)`"")
    if ($Ctx.NarrationOnly) { [void]$parts.Add('progress=none(narration)') }
    if ($Ctx.Kind -ne '') { [void]$parts.Add("kind=$($Ctx.Kind)") }
    return ($parts -join ' ')
}

function Format-VerdictLine {
    param($Ctx, [string]$Class, [string]$Note = '')
    $exitText = 'exit=n/a'
    if ($null -ne $Ctx.ExitCode) {
        $exitText = "exit=$(Format-ExitHex -Code ([int64]$Ctx.ExitCode)) ($($Ctx.ExitCode))"
        if ($Ctx.ExitSource -ne '') { $exitText += " source=$($Ctx.ExitSource)" }
    }
    $line = "VERDICT: $Class $exitText wall=$(Format-Number $Ctx.Wall)s " +
            "milestone=`"$($Ctx.LastMilestone)`" silent=$(Format-Number $Ctx.Silence)s " +
            "last=`"$(Shorten-Line -Text $Ctx.LastMeaningful -Max 160)`""
    if ($Ctx.Pid) { $line += " pid=$($Ctx.Pid)" }
    if ($Ctx.Kind -ne '') { $line += " kind=$($Ctx.Kind)" }
    if ($Ctx.Attempt -gt 1) { $line += " attempt=$($Ctx.Attempt)" }
    if ($Note -ne '') { $line += " note=$Note" }
    return $line
}

function Invoke-Watch {
    $script:WatchStartUtc = [DateTime]::UtcNow
    $watchClock = [System.Diagnostics.Stopwatch]::StartNew()

    $script:RunsDir = Join-Path $script:RootAbs '_Runs'
    $script:LogsDir = Join-Path $script:RootAbs '_Logs'
    $script:KytyPath = Join-Path $script:RunsDir (Join-Path $LogName '_kyty.txt')
    $script:StdoutPath = Join-Path $script:LogsDir ($LogName + '.log')
    $script:StderrPath = Join-Path $script:LogsDir ($LogName + '.stderr.log')
    $script:PidPath = Join-Path $script:LogsDir ($LogName + '.pid')
    $script:ResultPath = Join-Path $script:LogsDir ($LogName + '.result.json')
    $script:CrashDir = Join-Path $script:RunsDir (Join-Path $LogName 'crash')
    $script:StreamPaths = @{
        kyty   = $script:KytyPath
        stdout = $script:StdoutPath
        stderr = $script:StderrPath
    }

    $script:Proc = @{
        Pid            = $null
        Known          = $false
        Alive          = $false
        Handle         = [IntPtr]::Zero
        StartUtc       = $null
        ExitCode       = $null
        ExitSource     = ''
        DeadSince      = $null
        CpuLast        = $null
        CpuLastAdvanceUtc = $null
        CpuAdvancing   = $true
        CpuFlatSeconds = 0.0
    }

    # Initial silence is measured from the last write, conservatively assuming
    # that write was meaningful.  This makes an attach to a completely frozen
    # run conclude immediately, while an attach to a run whose stream is still
    # being flushed with idle narration starts at zero.
    $initAt = [DateTime]::UtcNow
    $kytyInfo = Get-Item -LiteralPath $script:KytyPath -ErrorAction SilentlyContinue
    if ($null -ne $kytyInfo) {
        $initAt = $kytyInfo.LastWriteTimeUtc
        if ($initAt -gt [DateTime]::UtcNow) { $initAt = [DateTime]::UtcNow }
    }
    $analysis = New-AnalysisState -At $initAt

    # Only a stream that existed once can "go missing": an uncreated stdout log
    # is normal at attach, not a state change worth printing.
    $streamSeen = @{ kyty = $false; stdout = $false; stderr = $false }

    # Open existing streams at their current length (tail-only).  Streams
    # created after the watcher starts are read from byte 0.
    $script:Readers = @{}
    foreach ($key in $script:StreamKeys) {
        $script:Readers[$key] = $null
        $path = $script:StreamPaths[$key]
        if (Test-Path -LiteralPath $path) {
            try {
                $script:Readers[$key] = New-ReaderState -Path $path -FromStart:$false
                $streamSeen[$key] = $true
            } catch {
                $script:Readers[$key] = $null
            }
        }
    }

    [void](Update-ProcState -Now $script:WatchStartUtc)

    $anyLog = (Test-Path -LiteralPath $script:KytyPath) -or
              (Test-Path -LiteralPath $script:StdoutPath) -or
              (Test-Path -LiteralPath $script:StderrPath)
    Emit ("MONITOR run={0} root={1} quiet={2}s poll={3}ms follow={4} tail-only=yes" -f
        $LogName, $script:RootAbs, $QuietAfter, $PollMs, [bool]$Follow)
    if (-not $anyLog) { Emit "MONITOR no streams yet (grace ${script:NoLogGrace}s)" }

    $lastStateClass = ''
    $lastVerdictClass = ''
    $lastStallClass = ''
    $attempt = 1
    $drained = $false
    $followDeadline = $null
    $finalClass = $null
    # The launcher rotates _kyty.txt before it writes the new pid.  Remember
    # when the rotation was seen so the pid change does not reset (and lose)
    # the lines already read from the new stream.
    $lastKytyRestartAt = $null
    $lastStreamMissing = @{ kyty = $false; stdout = $false; stderr = $false }
    # A rotated stream can briefly disappear between Move-Item and the create.
    # If that happens to _kyty.txt, the reopened file is a new attempt's stream,
    # so its milestone state must not inherit the old attempt's progress.
    $reopenReset = @{ kyty = $false; stdout = $false; stderr = $false }

    try {
        while ($true) {
            $now = [DateTime]::UtcNow

            $pidChanged = Update-ProcState -Now $now
            if ($pidChanged) {
                $attempt++
                $drained = $false
                $lastVerdictClass = ''
                $lastStallClass = ''
                $followDeadline = $null
                # Rotation usually precedes the pid write and has already
                # reset the analysis; resetting again here would discard the
                # new attempt's first lines.
                $recentRotation = ($null -ne $lastKytyRestartAt) -and
                    (($now - $lastKytyRestartAt).TotalSeconds -lt 5)
                if (-not $recentRotation) {
                    Reset-Analysis -Analysis $analysis -At $now
                }
                $reopenReset = @{ kyty = $false; stdout = $false; stderr = $false }
                Emit ("STATE restart attempt={0} pid={1}" -f $attempt, $script:Proc.Pid)
            }

            # --- read new bytes from every stream -------------------------
            foreach ($key in $script:StreamKeys) {
                $reader = $script:Readers[$key]
                if ($null -eq $reader) {
                    $path = $script:StreamPaths[$key]
                    if (Test-Path -LiteralPath $path) {
                        try {
                            $script:Readers[$key] = New-ReaderState -Path $path -FromStart
                            $reader = $script:Readers[$key]
                            $streamSeen[$key] = $true
                            Emit ("STATE stream-open stream={0} from=start" -f $key)
                            if ($reopenReset[$key]) {
                                $reopenReset[$key] = $false
                                if ($key -eq 'kyty') {
                                    Reset-Analysis -Analysis $analysis -At $now
                                    Emit 'STATE analysis-reset reason=kyty-reopen'
                                }
                            }
                        } catch {
                            $reader = $null
                        }
                    }
                }
                if ($null -eq $reader) {
                    if ($streamSeen[$key] -and -not $lastStreamMissing[$key] -and
                        (Test-Path -LiteralPath $script:RunsDir)) {
                        Emit ("STATE stream-missing stream={0}" -f $key)
                        $lastStreamMissing[$key] = $true
                    }
                    continue
                }
                $lastStreamMissing[$key] = $false

                $rr = Read-ReaderIntoAnalysis -Reader $reader -Analysis $analysis -Now $now `
                    -ResetOnRestart:($key -eq 'kyty')
                if ($rr.Missing) {
                    Emit ("STATE stream-missing stream={0}" -f $key)
                    $reopenReset[$key] = $true
                    $script:Readers[$key] = $null
                    continue
                }
                if ($rr.Error) {
                    Emit ("STATE stream-error stream={0} error=`"{1}`"" -f $key, (Shorten-Line $rr.Error 120))
                    Close-Reader -Reader $reader
                    $script:Readers[$key] = $null
                    continue
                }
                if ($rr.Restarted) {
                    Emit ("STATE stream-restart stream={0} restarts={1} bytes={2}" -f $key, $reader.Restarts, $rr.Bytes)
                    if ($key -eq 'kyty') {
                        # Rotation is part of a launcher relaunch, not an
                        # attempt of its own: the pid change counts it.
                        $lastKytyRestartAt = $now
                        $drained = $false
                        # While dead, the rotation is the start of a relaunch
                        # whose pid change will re-arm verdicts; re-arming here
                        # would print a duplicate "gone" for the same death.
                        if ($script:Proc.Alive) { $lastVerdictClass = '' }
                        $lastStallClass = ''
                        $followDeadline = $null
                    }
                }
                if ($rr.Skipped -gt 0) {
                    Emit ("STATE backlog-skip stream={0} skipped={1} bytes" -f $key, $rr.Skipped)
                }
            }

            if ($null -ne $script:MilestoneJustAdvanced) {
                Emit ("MILESTONE {0} ({1}/{2})" -f $script:MilestoneJustAdvanced,
                    ($analysis.MilestoneIndex + 1), $script:Milestones.Count)
                $script:MilestoneJustAdvanced = $null
            }

            # --- final drain after death ----------------------------------
            # The window/guest writes its last lines just before exiting.  Read
            # a few more times so "Window N closed"/"Event: quit" is not lost.
            if ($script:Proc.Known -and -not $script:Proc.Alive -and -not $drained) {
                Start-Sleep -Milliseconds 150
                for ($pass = 0; $pass -lt 3; $pass++) {
                    $total = [int64]0
                    foreach ($key in $script:StreamKeys) {
                        $r = $script:Readers[$key]
                        if ($null -ne $r) {
                            $dr = Read-ReaderIntoAnalysis -Reader $r -Analysis $analysis `
                                -Now ([DateTime]::UtcNow) -ResetOnRestart:($key -eq 'kyty')
                            $total += $dr.Bytes
                        }
                    }
                    if ($total -le 0) { break }
                    Start-Sleep -Milliseconds 100
                }
                $drained = $true
            }

            $silence = ($now - $analysis.LastMeaningfulAt).TotalSeconds
            if ($silence -lt 0) { $silence = 0.0 }

            $wallStart = $script:Proc.StartUtc
            if ($null -eq $wallStart) {
                $ki = Get-Item -LiteralPath $script:KytyPath -ErrorAction SilentlyContinue
                if ($null -ne $ki) { $wallStart = $ki.CreationTimeUtc }
            }
            if ($null -eq $wallStart) { $wallStart = $script:WatchStartUtc }
            $wall = ($now - $wallStart).TotalSeconds
            if ($wall -lt 0) { $wall = 0.0 }

            $logExists = (Test-Path -LiteralPath $script:KytyPath) -or
                         (Test-Path -LiteralPath $script:StdoutPath) -or
                         (Test-Path -LiteralPath $script:StderrPath)

            $timeLimitHit = ($Seconds -gt 0 -and $watchClock.Elapsed.TotalSeconds -ge $Seconds)

            # Terminal flush: a dying run often leaves its last line without a
            # newline ("Window 2 closed", a fault marker).  Only flush once the
            # stream can no longer grow (after the death drain) or when the
            # watcher itself is concluding on its wall limit.
            if ($timeLimitHit -or ($drained -and $script:Proc.Known -and -not $script:Proc.Alive)) {
                foreach ($key in $script:StreamKeys) {
                    $fr = $script:Readers[$key]
                    if ($null -ne $fr) {
                        [void](Flush-ReaderPending -Reader $fr -Analysis $analysis -Now $now)
                    }
                }
                $silence = ($now - $analysis.LastMeaningfulAt).TotalSeconds
                if ($silence -lt 0) { $silence = 0.0 }
            }

            $class = Get-Verdict -LogExists $logExists -ProcessKnown $script:Proc.Known `
                -ProcessAlive $script:Proc.Alive -ExitCode $script:Proc.ExitCode `
                -CpuAdvancing $script:Proc.CpuAdvancing -SilenceSeconds $silence `
                -QuietAfter $QuietAfter -SawUserClose $analysis.UserClose `
                -MeaningfulLines $analysis.MeaningfulLines -LastMilestone $analysis.MilestoneName `
                -TimeLimitHit $timeLimitHit -SinceStartSeconds $watchClock.Elapsed.TotalSeconds

            # -1 is how Stop-Process leaves a killed process behind.  If we
            # already concluded stall/hang for this attempt, keep that verdict
            # instead of relabelling the launcher's kill as a host crash.
            if ($class -eq 'gone' -and $lastStallClass -ne '' -and
                $null -ne $script:Proc.ExitCode -and [int64]$script:Proc.ExitCode -eq -1) {
                $class = $lastStallClass
            }

            $narrationOnly = ($analysis.RawLines -gt $analysis.RawAtLastMeaningful)
            $kind = ''
            if ($class -eq 'stall' -or $class -eq 'hang') {
                if ($narrationOnly) {
                    $kind = 'narration-only'
                } elseif ($analysis.RawLines -eq 0) {
                    $kind = 'no-output'
                } else {
                    $kind = 'quiet'
                }
                if ($script:Proc.CpuAdvancing) { $kind += ',cpu-spinning' } else { $kind += ',cpu-flat' }
            } elseif ($class -eq 'crash') {
                $kind = Get-CrashKind -Code ([int64]$script:Proc.ExitCode)
            } elseif ($class -eq 'gone' -and $null -ne $script:Proc.ExitCode) {
                $kind = 'terminated-externally'
            }

            $ctx = New-Object psobject -Property @{
                Class          = $class
                ExitCode       = $script:Proc.ExitCode
                ExitSource     = $script:Proc.ExitSource
                Wall           = $wall
                Silence        = $silence
                LastMilestone  = $analysis.MilestoneName
                LastMeaningful = $analysis.LastMeaningful
                MeaningfulLines = $analysis.MeaningfulLines
                RawLines       = $analysis.RawLines
                UserClose      = $analysis.UserClose
                ProcessAlive   = $script:Proc.Alive
                CpuAdvancing   = $script:Proc.CpuAdvancing
                Pid            = $script:Proc.Pid
                Attempt        = $attempt
                Kind           = $kind
                NarrationOnly  = $narrationOnly
                LogCrashMarker = $false
            }

            if ($class -ne $lastStateClass) {
                Emit ("STATE {0} {1}" -f $class, (Format-Metrics -Ctx $ctx))
                $lastStateClass = $class
            }

            $processGone = ($script:Proc.Known -and -not $script:Proc.Alive) -or
                           ((-not $script:Proc.Known) -and ($class -eq 'gone' -or $class -eq 'no-log'))
            $isStall = ($class -eq 'stall' -or $class -eq 'hang')
            if ($isStall) { $lastStallClass = $class }

            # A recovery re-arms verdict emission: a second stall later in the
            # same attempt is a new event, not a duplicate line.
            if ($processGone -eq $false -and (-not $isStall) -and $class -ne 'crash') {
                $lastVerdictClass = ''
            }

            $terminal = $processGone -or $isStall -or $timeLimitHit -or ($class -eq 'no-log')
            if ($terminal) {
                # Artefacts and the verdict belong to a verdict *transition*,
                # not to every poll of the same dead state (matters under
                # -Follow, where the loop keeps running after the verdict).
                $newVerdict = ($class -ne $lastVerdictClass)
                if ($class -eq 'crash' -and $newVerdict) {
                    # Last chance to spot a text-level host fault in the tail.
                    $tailText = (@(Get-FileTailLines -Path $script:StderrPath -Count 50 -MaxBytes (256KB)) -join "`n")
                    $ctx.LogCrashMarker = $script:CrashMarkerRegex.IsMatch($tailText)
                    $art = $null
                    try {
                        $art = Write-CrashArtifacts -Context $ctx -Dir $script:CrashDir `
                            -KytyPath $script:KytyPath -StdoutPath $script:StdoutPath `
                            -StderrPath $script:StderrPath -LogName $LogName
                    } catch {
                        Emit ("WARN crash-artifacts-failed error=`"{0}`"" -f (Shorten-Line $_.Exception.Message 160))
                    }
                    if ($null -ne $art) { Emit "ARTIFACTS $art" }
                }

                if ($newVerdict) {
                    $note = ''
                    if ($timeLimitHit) { $note = 'time-limit' }
                    Emit (Format-VerdictLine -Ctx $ctx -Class $class -Note $note)
                    $lastVerdictClass = $class
                }

                if (-not $Follow) {
                    $finalClass = $class
                    break
                }
                if ($class -eq 'no-log') {
                    $finalClass = $class
                    break
                }
                if ($processGone -and $null -eq $followDeadline) {
                    $followDeadline = $now.AddSeconds($script:FollowGrace)
                }
            }

            if ($timeLimitHit) {
                # No terminal state and the watcher's wall limit was reached:
                # report the live health honestly and leave the emulator alone.
                $finalClass = $class
                if ($class -ne $lastVerdictClass) {
                    Emit (Format-VerdictLine -Ctx $ctx -Class $class -Note 'time-limit')
                    $lastVerdictClass = $class
                }
                break
            }

            if ($null -ne $followDeadline -and $now -ge $followDeadline) {
                if ($lastVerdictClass -ne '') { $finalClass = $lastVerdictClass }
                else { $finalClass = $class }
                break
            }

            Start-Sleep -Milliseconds $PollMs
        }
    } finally {
        foreach ($key in $script:StreamKeys) {
            if ($null -ne $script:Readers -and $null -ne $script:Readers[$key]) {
                Close-Reader -Reader $script:Readers[$key]
            }
        }
        Close-ProcHandle
    }

    if ($null -eq $finalClass) { $finalClass = 'running' }
    return (Get-ClassExitCode -Class $finalClass)
}

# ---------------------------------------------------------------------------
# Self-test: synthetic streams for every classification outcome
# ---------------------------------------------------------------------------

function Invoke-SelfTest {
    $tempRoot = Join-Path ([System.IO.Path]::GetTempPath()) ('kyty-monitor-selftest-' + [guid]::NewGuid().ToString('N'))
    [void](New-Item -ItemType Directory -Path $tempRoot -Force)
    $script:SelfTestRoot = $tempRoot
    $pass = 0
    $fail = 0

    function Add-Case {
        param(
            [string]$Name,
            [string]$Kyty = '',
            [string]$Stdout = '',
            [string]$Stderr = '',
            $ExitCode = $null,
            [bool]$Alive = $false,
            [bool]$Known = $true,
            [bool]$CpuAdvancing = $true,
            [double]$Silence = 300.0,
            [bool]$LogExists = $true,
            [double]$SinceStart = 30.0,
            [string]$Expected,
            [double]$QuietAfter = 120.0,
            [bool]$Conclude = $false
        )
        return (New-Object psobject -Property @{
            Name         = $Name
            Kyty         = $Kyty
            Stdout       = $Stdout
            Stderr       = $Stderr
            ExitCode     = $ExitCode
            Alive        = $Alive
            Known        = $Known
            CpuAdvancing = $CpuAdvancing
            Silence      = $Silence
            LogExists    = $LogExists
            SinceStart   = $SinceStart
            Expected     = $Expected
            QuietAfter   = $QuietAfter
            Conclude     = $Conclude
        })
    }

    $crashLines = @(
        'PUI init',
        'scene load',
        '_sceRnpsBundleRegistryInit',
        'PopulatePool',
        'TTPoolInstance',
        'JSExecutorPreloadHostDll',
        'Evaluating bundle',
        'EventPluginRegistry',
        'canvas submit frame=1',
        '--- Error ---',
        'Unhandled host exception: type=1 code=3221225477 pc=0x00007ff90a17ceed access=2 address=0x000000010001043a'
    )

    $healthyLines = @(
        'PUI init',
        'scene load',
        '_sceRnpsBundleRegistryInit',
        'PopulatePool',
        'TTPoolInstance',
        'JSExecutorPreloadHostDll',
        'Evaluating bundle',
        'EventPluginRegistry',
        'canvas submit frame=1',
        'canvas present frame=1',
        'frame draw frame=1'
    )

    $cases = @(
        (Add-Case -Name 'crash-av' -Kyty (($crashLines + (1..450 | ForEach-Object { "filler line $_" })) -join "`n") `
            -ExitCode (-1073741819) -Alive $false -Expected 'crash' -Silence 0.4),
        (Add-Case -Name 'crash-stack-overflow' -Kyty 'PUI init`n--- Error ---`nSTACK_OVERFLOW tid=1' `
            -ExitCode (-1073741571) -Alive $false -Expected 'crash' -Silence 0.1),
        (Add-Case -Name 'guest-exit-321' -Kyty 'PUI init`nEventPluginRegistry`nguest requested exit' `
            -ExitCode 321 -Alive $false -Expected 'guest-exit' -Silence 0.2),
        (Add-Case -Name 'user-close' -Kyty 'PUI init`nWindow 2 closed`nEvent: quit' `
            -ExitCode 0 -Alive $false -Expected 'user-close' -Silence 0.2),
        (Add-Case -Name 'user-close-then-crash' -Kyty 'PUI init`nWindow 2 closed`nEvent: quit' `
            -ExitCode (-1073741819) -Alive $false -Expected 'user-close' -Silence 0.2),
        (Add-Case -Name 'freeze-no-output' -Kyty '' -ExitCode $null -Alive $true `
            -CpuAdvancing $true -Silence 300.0 -Expected 'stall'),
        (Add-Case -Name 'stall-with-narration' `
            -Kyty ("PUI init`n" + (@(
                "`tEqueue wait: ScePssWaitVBlank, thread_id = 5",
                "`t flipPendingNum = 0",
                'ps5-vsh: setspecific thread=SceSticker key=4 value=0x2ea5a8000',
                "`t regs     = 0x0000000ff803e89c",
                'CP packet: offset=0x00000 cmd_id=0xc002102c op=0x10 len=4',
                'Mono: mono_w32handle_wait_one: waiting',
                '[SceSoCPowerWatch] tick',
                'PthreadCondWait: parked'
            ) -join "`n")) `
            -ExitCode $null -Alive $true -CpuAdvancing $true -Silence 300.0 -Expected 'stall'),
        (Add-Case -Name 'healthy-progress' -Kyty ($healthyLines -join "`n") `
            -ExitCode $null -Alive $true -CpuAdvancing $true -Silence 2.0 -Expected 'ok' -Conclude $true),
        (Add-Case -Name 'hang-cpu-flat' -Kyty 'PUI init' -ExitCode $null -Alive $true `
            -CpuAdvancing $false -Silence 300.0 -Expected 'hang'),
        (Add-Case -Name 'gone-no-exit-code' -Kyty 'PUI init' -ExitCode $null -Alive $false -Expected 'gone'),
        (Add-Case -Name 'external-termination' -Kyty 'PUI init' -ExitCode (-1) -Alive $false -Expected 'gone'),
        (Add-Case -Name 'no-log' -ExitCode $null -Alive $false -LogExists $false -SinceStart 30.0 -Expected 'no-log')
    )

    $created = @{}
    foreach ($case in $cases) {
        $runDir = Join-Path $tempRoot (Join-Path '_Runs' $case.Name)
        $logDir = Join-Path $tempRoot '_Logs'
        [void](New-Item -ItemType Directory -Path $runDir -Force)
        [void](New-Item -ItemType Directory -Path $logDir -Force)
        $kytyPath = Join-Path $runDir '_kyty.txt'
        $stdoutPath = Join-Path $logDir ($case.Name + '.log')
        $stderrPath = Join-Path $logDir ($case.Name + '.stderr.log')
        if ($case.LogExists) {
            Write-Utf8Text -Path $kytyPath -Text $case.Kyty
            Write-Utf8Text -Path $stdoutPath -Text $case.Stdout
            Write-Utf8Text -Path $stderrPath -Text $case.Stderr
        }
        $created[$case.Name] = @{
            Kyty = $kytyPath; Stdout = $stdoutPath; Stderr = $stderrPath; RunDir = $runDir
        }
    }

    try {
        foreach ($case in $cases) {
            $paths = $created[$case.Name]
            $now = [DateTime]::UtcNow
            $analysis = New-AnalysisState -At $now
            foreach ($p in @($paths.Kyty, $paths.Stdout, $paths.Stderr)) {
                if (-not (Test-Path -LiteralPath $p)) { continue }
                $r = New-ReaderState -Path $p -FromStart
                try {
                    for ($i = 0; $i -lt 50; $i++) {
                        $rr = Read-ReaderIntoAnalysis -Reader $r -Analysis $analysis -Now $now `
                            -MaxBytes (16MB) -MaxLagBytes (64MB) -LagKeepBytes (16MB)
                        if ($rr.Bytes -le 0) { break }
                    }
                    # Synthetic streams are terminal for this case: honour a
                    # final line that has no trailing newline.
                    [void](Flush-ReaderPending -Reader $r -Analysis $analysis -Now $now)
                } finally {
                    Close-Reader -Reader $r
                }
            }

            $logExists = (Test-Path -LiteralPath $paths.Kyty) -or
                         (Test-Path -LiteralPath $paths.Stdout) -or
                         (Test-Path -LiteralPath $paths.Stderr)
            if ($case.Name -eq 'no-log') { $logExists = $false }

            $actual = Get-Verdict -LogExists $logExists -ProcessKnown $case.Known `
                -ProcessAlive $case.Alive -ExitCode $case.ExitCode `
                -CpuAdvancing $case.CpuAdvancing -SilenceSeconds $case.Silence `
                -QuietAfter $case.QuietAfter -SawUserClose $analysis.UserClose `
                -MeaningfulLines $analysis.MeaningfulLines -LastMilestone $analysis.MilestoneName `
                -TimeLimitHit $case.Conclude -SinceStartSeconds $case.SinceStart

            $ok = ($actual -eq $case.Expected)
            $extra = ''
            if ($ok -and $case.Name -eq 'stall-with-narration') {
                if ($analysis.MeaningfulLines -ne 1) {
                    $ok = $false
                    $extra = " meaningful=$($analysis.MeaningfulLines) (expected 1)"
                } elseif ($analysis.IgnoredLines -lt 8) {
                    $ok = $false
                    $extra = " ignored=$($analysis.IgnoredLines) (expected >=8)"
                }
            }
            if ($ok -and $case.Name -eq 'healthy-progress') {
                if ($analysis.MilestoneIndex -ne 8) {
                    $ok = $false
                    $extra = " milestoneIndex=$($analysis.MilestoneIndex) (expected 8)"
                }
            }
            if ($ok -and $case.Name -eq 'user-close') {
                if (-not $analysis.UserClose) {
                    $ok = $false
                    $extra = ' userCloseMarker not seen'
                }
            }
            if ($ok -and $case.Name -eq 'crash-av') {
                # Exercise the real artefact writer (the only write path).
                $crashDir = Join-Path $paths.RunDir 'crash'
                $ctx = New-Object psobject -Property @{
                    ExitCode = $case.ExitCode; Kind = (Get-CrashKind -Code ([int64]$case.ExitCode))
                    ExitSource = 'selftest'; Wall = 12.3; LastMilestone = $analysis.MilestoneName
                    Silence = $case.Silence; LastMeaningful = $analysis.LastMeaningful
                    MeaningfulLines = $analysis.MeaningfulLines; RawLines = $analysis.RawLines
                    UserClose = $analysis.UserClose; Pid = 1234; Attempt = 1; LogCrashMarker = $true
                }
                try {
                    $null = Write-CrashArtifacts -Context $ctx -Dir $crashDir `
                        -KytyPath $paths.Kyty -StdoutPath $paths.Stdout -StderrPath $paths.Stderr `
                        -LogName $case.Name
                    foreach ($f in @('verdict.txt', 'kyty_tail.txt', 'stderr_tail.txt', 'stdout_tail.txt')) {
                        if (-not (Test-Path -LiteralPath (Join-Path $crashDir $f))) {
                            $ok = $false; $extra += " missing=$f"
                        }
                    }
                    $vText = [System.IO.File]::ReadAllText((Join-Path $crashDir 'verdict.txt'))
                    if ($vText -notmatch '0xC0000005') {
                        $ok = $false; $extra += ' verdict.txt lacks 0xC0000005'
                    }
                    $kytyTail = @(Get-Content -LiteralPath (Join-Path $crashDir 'kyty_tail.txt'))
                    if ($kytyTail.Count -ne 400) {
                        $ok = $false; $extra += " kyty_tail count=$($kytyTail.Count) (expected 400)"
                    } elseif ($kytyTail[0] -ne 'filler line 51') {
                        # 11 milestone/error lines + 450 filler = 461; the last
                        # 400 therefore begin at filler line 51.
                        $ok = $false; $extra += " kyty_tail[0]='$($kytyTail[0])' (expected 'filler line 51')"
                    }
                } catch {
                    $ok = $false; $extra += " artefacts: $($_.Exception.Message)"
                }
            }

            if ($ok) {
                $pass++
                Emit ("SELFTEST {0}: PASS (expected={1} actual={2} meaningful={3} milestone={4})" -f
                    $case.Name, $case.Expected, $actual, $analysis.MeaningfulLines, $analysis.MilestoneName)
            } else {
                $fail++
                Emit ("SELFTEST {0}: FAIL (expected={1} actual={2} meaningful={3} milestone={4}){5}" -f
                    $case.Name, $case.Expected, $actual, $analysis.MeaningfulLines,
                    $analysis.MilestoneName, $extra)
            }
        }

        # --- reader resync tests (truncation, rotation, split line) --------
        $readerDir = Join-Path $tempRoot '_Runs\reader'
        [void](New-Item -ItemType Directory -Path $readerDir -Force)
        $rp = Join-Path $readerDir '_kyty.txt'
        $rot = New-AnalysisState -At ([DateTime]::UtcNow)
        # Deliberately longer than the post-truncation content: truncation is
        # only observable as a shrink (a whitespace-only pad is not a line).
        Write-Utf8Text -Path $rp -Text ("PUI init`n" + (' ' * 200) + "`n")
        $r = New-ReaderState -Path $rp -FromStart
        try {
            $rr = Read-ReaderIntoAnalysis -Reader $r -Analysis $rot -Now ([DateTime]::UtcNow) -ResetOnRestart $true
            $rotOk = ($rot.MeaningfulLines -eq 1 -and $rot.MilestoneIndex -eq 0)
            $rotExtra = " initial meaningful=$($rot.MeaningfulLines) milestone=$($rot.MilestoneIndex)"

            # In-place truncation: the emulator relaunch truncates _kyty.txt.
            Write-Utf8Text -Path $rp -Text "scene load`n"
            $rr = Read-ReaderIntoAnalysis -Reader $r -Analysis $rot -Now ([DateTime]::UtcNow) -ResetOnRestart $true
            if (-not $rr.Restarted) { $rotOk = $false; $rotExtra += ' truncation-not-detected' }
            if ($rot.MeaningfulLines -ne 1 -or $rot.MilestoneIndex -ne 1) {
                $rotOk = $false; $rotExtra += " after-truncate meaningful=$($rot.MeaningfulLines) milestone=$($rot.MilestoneIndex)"
            }

            # Rotation: Move-Item <name>.log <name>.log.stall1, then a new file.
            Move-Item -LiteralPath $rp -Destination ($rp + '.stall1') -Force
            Write-Utf8Text -Path $rp -Text "PopulatePool`n"
            $rr = Read-ReaderIntoAnalysis -Reader $r -Analysis $rot -Now ([DateTime]::UtcNow) -ResetOnRestart $true
            if (-not $rr.Restarted) { $rotOk = $false; $rotExtra += ' rotation-not-detected' }
            if ($rot.MilestoneIndex -ne 3) {
                $rotOk = $false; $rotExtra += " after-rotate milestone=$($rot.MilestoneIndex)"
            }

            # Partial line must be joined, not emitted twice or dropped.
            $sp = Join-Path $readerDir 'split.txt'
            Write-Utf8Text -Path $sp -Text 'abc'
            $sr = New-ReaderState -Path $sp -FromStart
            $sa = New-AnalysisState -At ([DateTime]::UtcNow)
            $null = Read-ReaderIntoAnalysis -Reader $sr -Analysis $sa -Now ([DateTime]::UtcNow)
            $splitOk = ($sa.RawLines -eq 0)
            $fs = [System.IO.File]::Open($sp, [System.IO.FileMode]::Append, [System.IO.FileAccess]::Write, [System.IO.FileShare]::ReadWrite)
            try {
                $bytes = [System.Text.Encoding]::UTF8.GetBytes("def`n")
                $fs.Write($bytes, 0, $bytes.Length)
            } finally { $fs.Dispose() }
            $null = Read-ReaderIntoAnalysis -Reader $sr -Analysis $sa -Now ([DateTime]::UtcNow)
            if ($sa.RawLines -ne 1 -or $sa.LastMeaningful -ne 'abcdef') {
                $splitOk = $false
                $rotExtra += " split raw=$($sa.RawLines) last='$($sa.LastMeaningful)'"
            }
            Close-Reader -Reader $sr
        } finally {
            Close-Reader -Reader $r
        }

        # --- result.json fallback (attach after the process handle is gone) --
        $rj = Join-Path $tempRoot '_Logs\rj.result.json'
        Write-Utf8Text -Path $rj -Text '{"pid":4242,"exitCode":-1073741819}'
        $rjOk = $true
        $rjExtra = ''
        if ((Read-ResultExitCode -Path $rj -ProcessId 4242) -ne (-1073741819)) {
            $rjOk = $false; $rjExtra += ' matching-pid-not-read'
        }
        if ($null -ne (Read-ResultExitCode -Path $rj -ProcessId 9999)) {
            $rjOk = $false; $rjExtra += ' mismatched-pid-accepted'
        }

        if ($rotOk) { $pass++; Emit 'SELFTEST reader-resync: PASS' }
        else { $fail++; Emit ("SELFTEST reader-resync: FAIL{0}" -f $rotExtra) }
        if ($splitOk) { $pass++; Emit 'SELFTEST split-line: PASS' }
        else { $fail++; Emit ("SELFTEST split-line: FAIL{0}" -f $rotExtra) }
        if ($rjOk) { $pass++; Emit 'SELFTEST result-json: PASS' }
        else { $fail++; Emit ("SELFTEST result-json: FAIL{0}" -f $rjExtra) }
    } finally {
        Remove-Item -LiteralPath $tempRoot -Recurse -Force -ErrorAction SilentlyContinue
    }

    Emit ("SELFTEST RESULT: {0} passed, {1} failed" -f $pass, $fail)
    if ($fail -gt 0) { return 4 }
    return 0
}

# ---------------------------------------------------------------------------
# Main
# ---------------------------------------------------------------------------

if ($SelfTest) {
    exit (Invoke-SelfTest)
}

if ([string]::IsNullOrWhiteSpace($LogName)) {
    [Console]::Error.WriteLine('usage: monitor_shellui_run.ps1 -LogName <run> [-Seconds N] [-QuietAfter N] [-Follow] [-IgnorePattern REGEX]')
    [Console]::Error.WriteLine('       monitor_shellui_run.ps1 -SelfTest')
    exit 1
}
if ($LogName -notmatch '^[A-Za-z0-9][A-Za-z0-9._-]*$') {
    [Console]::Error.WriteLine("monitor: invalid -LogName '$LogName' (letters, digits, dot, underscore, dash)")
    exit 1
}
if (-not (Test-Path -LiteralPath $Root -PathType Container)) {
    [Console]::Error.WriteLine("monitor: -Root not found: $Root")
    exit 1
}
$script:RootAbs = (Resolve-Path -LiteralPath $Root).Path

exit (Invoke-Watch)


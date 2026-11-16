# Frame-counter watchdog for a visible kyty run.
#
# The emulator's window title is "[build | type] <title>, <id>, <ver> [gpu]
# [cpu], frame: N, fps: M" (src/graphics/presentation/window/window.cpp,
# WindowContext::UpdateTitle). N increments once per presented frame. A boot
# that is parked in the compositor/shell event loop keeps writing log bytes
# (CP packets, equeue polling, Mono ref-count narration) while N does not move,
# so log silence is not a reliable stuck signal. This watcher reads N from the
# window title and reports when it stops advancing.
#
# It also reads the guest's own compositor narration from _Runs/<log>/_kyty.txt:
#   found a pending flip. (N [M] pf=1 ...
# N is the guest flip counter. A healthy boot advances it every ~170 ms; the
# parked boot repeats the line with N frozen while "Equeue wait timedout:
# ScePssWaitVBlank" keeps appearing. flip counter frozen + log still growing +
# vblank timeouts is the compositor park, and it is visible seconds before the
# title counter has been still for StallSeconds.
#
# Read-only by default. -KillOnStall terminates the process that owns the
# window (the emulator), which is what a manual "kill it, it's stuck" does.
param(
    [string]$LogName = 'frame',
    [string]$Root = '',
    [ValidateRange(0, 86400)][int]$StallSeconds = 90,
    [ValidateRange(0, 86400)][int]$StartupSeconds = 150,
    [ValidateRange(100, 60000)][int]$PollMs = 500,
    [ValidateRange(0, 2147483647)][int]$EmuPid = 0,
    [ValidateRange(0, 86400)][int]$FlipParkSeconds = 10,
    [ValidateRange(0, 86400)][int]$NoveltyStallSeconds = 90,
    [ValidateRange(0, 100000)][int]$NoveltyRateMin = 60,
    [ValidateRange(1, 1000)][int]$NoveltyLowWindows = 6,
    [string]$GuestLog = '',
    [switch]$KillOnStall,
    [switch]$Follow,
    [ValidateRange(0, 86400)][int]$MaxSeconds = 0
)

$ErrorActionPreference = 'Stop'

if ([string]::IsNullOrEmpty($Root)) {
    $Root = (Resolve-Path (Join-Path $PSScriptRoot '..')).Path
}
$logDir = Join-Path $Root '_Logs'
if (-not (Test-Path -LiteralPath $logDir)) {
    New-Item -ItemType Directory -Path $logDir | Out-Null
}
$outPath = Join-Path $logDir ("{0}.frames.log" -f $LogName)

if (-not ('KytyFrames.Win32' -as [type])) {
    Add-Type -TypeDefinition @'
using System;
using System.Collections.Generic;
using System.Runtime.InteropServices;
using System.Text;

namespace KytyFrames {
    public class WindowInfo {
        public IntPtr Handle;
        public uint Pid;
        public string Title;
    }

    public static class Win32 {
        private delegate bool EnumWindowsProc(IntPtr hWnd, IntPtr lParam);

        [DllImport("user32.dll")]
        private static extern bool EnumWindows(EnumWindowsProc cb, IntPtr lParam);

        [DllImport("user32.dll", CharSet = CharSet.Unicode)]
        private static extern int GetWindowTextLength(IntPtr hWnd);

        [DllImport("user32.dll", CharSet = CharSet.Unicode)]
        private static extern int GetWindowText(IntPtr hWnd, StringBuilder text, int maxCount);

        [DllImport("user32.dll")]
        private static extern bool IsWindowVisible(IntPtr hWnd);

        [DllImport("user32.dll")]
        private static extern uint GetWindowThreadProcessId(IntPtr hWnd, out uint pid);

        public static List<WindowInfo> TopLevel() {
            var list = new List<WindowInfo>();
            EnumWindows(delegate(IntPtr h, IntPtr p) {
                if (!IsWindowVisible(h)) return true;
                int len = GetWindowTextLength(h);
                if (len <= 0) return true;
                var sb = new StringBuilder(len + 1);
                if (GetWindowText(h, sb, sb.Capacity) <= 0) return true;
                uint pid;
                GetWindowThreadProcessId(h, out pid);
                list.Add(new WindowInfo { Handle = h, Pid = pid, Title = sb.ToString() });
                return true;
            }, IntPtr.Zero);
            return list;
        }
    }
}
'@
}

function Write-FrameLog([string]$line) {
    $ts = (Get-Date).ToString('HH:mm:ss')
    "$ts $line" | Add-Content -LiteralPath $outPath
    Write-Output $line
}

# Kill target: prefer the emulator PID the launcher handed us; the window
# owner is unreliable under Parsec (it can resolve to dwm). Only ever stop
# a kyty* process.
function Stop-Emulator([int]$fallbackPid) {
    $target = if ($EmuPid -gt 0) { $EmuPid } else { $fallbackPid }
    try {
        $p = Get-Process -Id $target -ErrorAction Stop
        if ($p.ProcessName -notlike 'kyty*') {
            Write-FrameLog ("KILL-SKIPPED pid={0} name={1} (not the emulator)" -f $target, $p.ProcessName)
            return
        }
        Write-FrameLog ("KILL pid={0} name={1}" -f $target, $p.ProcessName)
        Stop-Process -Id $target -Force -ErrorAction Stop
    } catch {
        Write-FrameLog ("KILL-FAILED pid={0}: {1}" -f $target, $_.Exception.Message)
    }
}

# On a verdict, quote what the guest was doing: the parks are not all the same
# loop, so classify each one from its own narration instead of guessing.
function Write-TailHint([string]$path) {
    $tail = Read-LogTail $path 131072
    if ($null -eq $tail) { return }
    $pats = @('Equeue wait', 'found a pending flip', 'flipPendingNum', 'wait_reg_mem',
              'Error', 'error', 'EXIT', 'hang', 'timeout')
    $picked = New-Object System.Collections.Generic.List[string]
    foreach ($line in ($tail.Text -split "`n")) {
        foreach ($p in $pats) {
            if ($line.Contains($p)) { $picked.Add($line.Trim()); break }
        }
    }
    if ($picked.Count -eq 0) { return }
    $from = [Math]::Max(0, $picked.Count - 6)
    for ($i = $from; $i -lt $picked.Count; $i++) {
        $l = $picked[$i]
        if ($l.Length -gt 180) { $l = $l.Substring(0, 180) }
        Write-FrameLog ("TAIL {0}" -f $l)
    }
}

# The title is the only place the guest-side frame counter is published.
$rx = [regex]'frame:\s*(\d+),\s*fps:\s*(\d+)'

# Guest-log park signature (see the header).
if ([string]::IsNullOrEmpty($GuestLog)) {
    $GuestLog = Join-Path $Root ("_Runs/{0}/_kyty.txt" -f $LogName)
}
$flipRx   = [regex]'found a pending flip\. \((\d+) \['
$vblankRx = [regex]'Equeue wait timedout:'
$logLen   = [int64]-1
$flipIndex  = $null
$flipChange = Get-Date

# Spin detector. A parked boot keeps narrating the *same* few calls forever once
# pointers and numbers are normalised; a slow-but-healthy boot keeps producing
# new lines (module loads, Mono AOT methods, file paths). No new key for
# NoveltyStallSeconds while the log still grows is a park. A frozen frame
# counter is not: the shell stops presenting for minutes while it loads.
$novelSet    = @{}
$novelOffset = [int64]0
$novelCount  = [int64]0
$novelWindow = [int64]0
$bytesWindow = [int64]0
$lowWindows  = 0
$lastNovel   = Get-Date
$lastNovelLine = '(none)'
$lastMarkerLine = '(none)'
$lastMarkerAt = $null
$bootedAt = $null
$bootRx = [regex]'(OnOpenSession|pshomeui:main|ReadyGameBootNotified)'
$markerRx = [regex]('(?i)(\[RNPS\]|Mono:|Relocate program|Evaluating bundle|PreloadHostDll|' +
    'ReactNative|PUI|Present|EXIT|Error|Exception)')
$nextProgressAt = (Get-Date).AddSeconds(30)
$noiseRx = [regex](('(?i)(Equeue wait|flipPendingNum|PthreadCondWait|vm\.stats|CP packet|' +
    'setspecific|regs\s*=|OperatorNew|SceLocaleInit|layout-cmp|ps5-vsh trace|' +
    '^\s*(arg\d|cond|count|data|num_|increment|write_confirm|processTime|submit|' +
    'flipArg|gcQueueNum|currentBuffer|reserved)\b)'))
$hexRx = [regex]'0x[0-9a-fA-F]+'
$decRx = [regex]'\d+'
if (Test-Path -LiteralPath $GuestLog) {
    $novelOffset = (Get-Item -LiteralPath $GuestLog).Length
}

function Update-GuestNovelty([string]$path, [int]$maxLines) {
    try {
        $fs = [System.IO.File]::Open($path, [System.IO.FileMode]::Open,
                                     [System.IO.FileAccess]::Read,
                                     [System.IO.FileShare]::ReadWrite)
    } catch {
        return
    }
    try {
        $len = $fs.Length
        if ($len -le $script:novelOffset) { return }
        $null = $fs.Seek($script:novelOffset, [System.IO.SeekOrigin]::Begin)
        $take = [Math]::Min([int64]4194304, $len - $script:novelOffset)
        $buf  = New-Object byte[] $take
        $read = 0
        while ($read -lt $take) {
            $n = $fs.Read($buf, $read, $take - $read)
            if ($n -le 0) { break }
            $read += $n
        }
        $lastNl = -1
        for ($i = $read - 1; $i -ge 0; $i--) {
            if ($buf[$i] -eq 10) { $lastNl = $i; break }
        }
        if ($lastNl -lt 0) { return }
        $text = [System.Text.Encoding]::UTF8.GetString($buf, 0, $lastNl)
        $script:novelOffset += ($lastNl + 1)
        # Boot detection must not depend on the capped per-line scan below:
        # at ~700 KB/s the marker line can sit in a skipped region. Scan the
        # raw chunk (fast regex) and latch it.
        if ($null -eq $script:bootedAt -and $bootRx.IsMatch($text)) {
            $script:bootedAt = Get-Date
        }
        $processed = 0
        $added = 0
        foreach ($line in ($text -split "`n")) {
            if ($processed -ge $maxLines) { break }
            $processed++
            if ($line.Length -eq 0 -or $noiseRx.IsMatch($line)) { continue }
            $key = $decRx.Replace($hexRx.Replace($line, '0x'), 'N')
            if ($key.Length -gt 160) { $key = $key.Substring(0, 160) }
            if (-not $script:novelSet.ContainsKey($key)) {
                if ($script:novelSet.Count -ge 200000) { $script:novelSet.Clear() }
                $script:novelSet[$key] = 1
                $script:lastNovelLine = $line.Trim()
                if ($null -eq $script:bootedAt -and $bootRx.IsMatch($line)) {
                    $script:bootedAt = Get-Date
                }
                if ($markerRx.IsMatch($line)) {
                    $script:lastMarkerLine = $line.Trim()
                    $script:lastMarkerAt = Get-Date
                }
                $added++
            }
        }
        if ($added -gt 0) {
            $script:novelCount += $added
            $script:novelWindow += $added
            $script:lastNovel = Get-Date
        }
    } finally {
        $fs.Dispose()
    }
}

function Read-LogTail([string]$path, [int]$maxBytes) {
    try {
        $fs = [System.IO.File]::Open($path, [System.IO.FileMode]::Open,
                                     [System.IO.FileAccess]::Read,
                                     [System.IO.FileShare]::ReadWrite)
        try {
            $len  = $fs.Length
            $take = [Math]::Min([int64]$maxBytes, $len)
            $null = $fs.Seek($len - $take, [System.IO.SeekOrigin]::Begin)
            $buf  = New-Object byte[] $take
            $read = 0
            while ($read -lt $take) {
                $n = $fs.Read($buf, $read, $take - $read)
                if ($n -le 0) { break }
                $read += $n
            }
            return @{ Length = $len; Text = [System.Text.Encoding]::UTF8.GetString($buf, 0, $read) }
        } finally {
            $fs.Dispose()
        }
    } catch {
        return $null
    }
}

$started = Get-Date
$lastFrame = $null
$lastChange = Get-Date
$lastWrite = Get-Date
$firstSeen = $null
$loggedNoWindow = $false
$verdict = $null

Write-FrameLog ("WATCH log={0} stall={1}s startup={2}s poll={3}ms kill={4}" -f `
    $LogName, $StallSeconds, $StartupSeconds, $PollMs, [bool]$KillOnStall)

while ($true) {
    $now = Get-Date
    $windows = [KytyFrames.Win32]::TopLevel()
    $match = $null
    $win = $null
    foreach ($w in $windows) {
        $m = $rx.Match($w.Title)
        if ($m.Success) { $match = $m; $win = $w; break }
    }

    if ($null -eq $match) {
        if ($null -eq $firstSeen) {
            # Before the first present there is no "frame: N" in the title. That
            # is normal during startup, so report it once and keep polling; the
            # launcher owns process lifetime and stops this watcher.
            if (-not $loggedNoWindow -and $StartupSeconds -gt 0 -and
                ($now - $started).TotalSeconds -ge $StartupSeconds) {
                Write-FrameLog ("STATE no-window after {0:N0}s" -f ($now - $started).TotalSeconds)
                $loggedNoWindow = $true
            }
        } elseif ($Follow) {
            Write-FrameLog ("STATE window-gone frame={0}" -f $lastFrame)
            $verdict = 'window-gone'
        }
    } else {
        $frame = [int]$match.Groups[1].Value
        $fps = [int]$match.Groups[2].Value
        if ($null -eq $firstSeen) {
            $firstSeen = $now
            $lastChange = $now
            Write-FrameLog ("STATE window pid={0} frame={1} fps={2}" -f $win.Pid, $frame, $fps)
        }
        if ($null -eq $lastFrame -or $frame -ne $lastFrame) {
            if ($null -ne $lastFrame -and $frame -ne $lastFrame) {
                Write-FrameLog ("FRAME {0} -> {1} fps={2} dt={3:N1}s" -f $lastFrame, $frame, $fps, ($now - $lastChange).TotalSeconds)
            }
            $lastFrame = $frame
            $lastChange = $now
            $lastWrite = $now
        } elseif (($now - $lastWrite).TotalSeconds -ge 10) {
            Write-FrameLog ("STATE frame={0} fps={1} still={2:N0}s" -f $frame, $fps, ($now - $lastChange).TotalSeconds)
            $lastWrite = $now
        }

        if ($StallSeconds -gt 0 -and ($now - $lastChange).TotalSeconds -ge $StallSeconds) {
            $still = ($now - $lastChange).TotalSeconds
            Write-FrameLog ("VERDICT: frame-stall frame={0} fps={1} still={2:N0}s pid={3}" -f $frame, $fps, $still, $win.Pid)
            Write-TailHint $GuestLog
            if ($KillOnStall) {
                try {
                    Stop-Emulator $win.Pid
                } catch {
                    Write-FrameLog ("KILL-FAILED pid={0}: {1}" -f $win.Pid, $_.Exception.Message)
                }
            }
            $verdict = 'frame-stall'
        }
    }

    if ($null -ne $verdict) { break }

    if ($NoveltyStallSeconds -gt 0) {
        $before = $novelOffset
        Update-GuestNovelty $GuestLog 4000
        $grew = $novelOffset -gt $before
        # A shell that is still presenting is alive by definition, whatever
        # its log narration does. Never spin-park it (svcprobe13: three
        # booted attempts shot at frames 4060/3915/1511 while presenting).
        $frameFrozenSecs = 99999.0
        if ($null -ne $lastChange) { $frameFrozenSecs = ($now - $lastChange).TotalSeconds }
        $bytesWindow += ($novelOffset - $before)
        if ($now -ge $nextProgressAt) {
            $last = $lastNovelLine
            if ($null -ne $lastMarkerAt -and $lastMarkerAt -ge $lastNovel) { $last = $lastMarkerLine }
            if ($last.Length -gt 120) { $last = $last.Substring(0, 120) }
            $quiet = [int][Math]::Max(0, ($now - $lastNovel).TotalSeconds)
            $mb = $bytesWindow / 1MB
            if (($mb -ge 1 -or $quiet -ge 25) -and $novelWindow -lt $NoveltyRateMin) {
                $lowWindows++
            } else {
                $lowWindows = 0
            }
            Write-FrameLog ("STATE progress frame={0} novel+{1} total={2} quiet={3}s mb={4:N1} low={5} last='{6}'" -f `
                $lastFrame, $novelWindow, $novelCount, $quiet, $mb, $lowWindows, $last)
            $novelWindow = 0
            $bytesWindow = 0
            $nextProgressAt = $now.AddSeconds(30)
            if ($lowWindows -ge $NoveltyLowWindows -and $frameFrozenSecs -ge 180) {
                if ($quiet -lt 30) {
                    # A genuinely new line shape appeared in the last 30 s:
                    # the guest is working (heavy repetitive phases mint few
                    # shapes while writing megabytes). Not a park.
                    Write-FrameLog ("STATE spin-park suppressed: quiet={0}s (new shape recently) frame={1}" -f $quiet, $lastFrame)
                    $lowWindows = 0
                } elseif ($null -ne $bootedAt) {
                    # Success state: the shell booted (OpenSession/pshomeui
                    # seen) and is now idling at the home screen. Repetitive
                    # narration from here is health, not a park - stand down
                    # without killing (svcprobe6 was shot at frame 4358).
                    Write-FrameLog ("VERDICT: booted (watchdog standing down; spin-park suppressed after boot at {0})" -f $bootedAt.ToString('HH:mm:ss'))
                    $verdict = 'booted'
                    break
                } else {
                    # The log keeps growing but almost nothing in it is new:
                    # the guest is repeating a handful of calls (idle
                    # narration), not loading. This is the park that a frozen
                    # frame counter cannot distinguish from slow loading.
                    Write-FrameLog ("VERDICT: spin-park low-novelty windows={0} minRate={1} mb={2:N1} quiet={3}s titleFrame={4}" -f `
                        $lowWindows, $NoveltyRateMin, $mb, $quiet, $lastFrame)
                    Write-TailHint $GuestLog
                    if ($KillOnStall -and $null -ne $win) {
                        try {
                            Stop-Emulator $win.Pid
                        } catch {
                            Write-FrameLog ("KILL-FAILED pid={0}: {1}" -f $win.Pid, $_.Exception.Message)
                        }
                    }
                    $verdict = 'spin-park'
                }
            }
        }
        if ($null -eq $bootedAt -and $grew -and $frameFrozenSecs -ge 180 -and ($now - $lastNovel).TotalSeconds -ge $NoveltyStallSeconds) {
            Write-FrameLog ("VERDICT: spin-park novel={0} quiet={1:N0}s log={2} titleFrame={3}" -f `
                $novelCount, ($now - $lastNovel).TotalSeconds, $novelOffset, $lastFrame)
            Write-TailHint $GuestLog
            if ($KillOnStall -and $null -ne $win) {
                try {
                    Stop-Emulator $win.Pid
                } catch {
                    Write-FrameLog ("KILL-FAILED pid={0}: {1}" -f $win.Pid, $_.Exception.Message)
                }
            }
            $verdict = 'spin-park'
        }
    }

    if ($null -ne $verdict) { break }

    if ($FlipParkSeconds -gt 0) {
        $tail = Read-LogTail $GuestLog 1048576
        if ($null -ne $tail) {
            $grew = ($logLen -ge 0) -and ($tail.Length -gt $logLen)
            $logLen = $tail.Length
            $lastFlip = $null
            foreach ($mm in $flipRx.Matches($tail.Text)) { $lastFlip = $mm }
            $frozen = $false
            if ($null -ne $lastFlip) {
                $idx = [int]$lastFlip.Groups[1].Value
                if ($null -eq $flipIndex -or $idx -ne $flipIndex) {
                    $flipIndex  = $idx
                    $flipChange = $now
                } else {
                    $frozen = $true
                }
            } elseif ($null -ne $flipIndex) {
                # The flip line scrolled out of the window; keep the last index
                # and let the vblank-timeout evidence decide.
                $frozen = $true
            }
            $timeouts = $vblankRx.Matches($tail.Text).Count
            if ($frozen -and $grew -and $timeouts -ge 3 -and
                ($now - $flipChange).TotalSeconds -ge $FlipParkSeconds) {
                Write-FrameLog ("VERDICT: flip-park flip={0} frozen={1:N0}s vblankTimeouts={2} log={3} titleFrame={4}" -f `
                    $flipIndex, ($now - $flipChange).TotalSeconds, $timeouts, $tail.Length, $lastFrame)
                if ($KillOnStall -and $null -ne $win) {
                    try {
                        Stop-Emulator $win.Pid
                    } catch {
                        Write-FrameLog ("KILL-FAILED pid={0}: {1}" -f $win.Pid, $_.Exception.Message)
                    }
                }
                $verdict = 'flip-park'
            }
        }
    }

    if ($null -ne $verdict) { break }
    if ($MaxSeconds -gt 0 -and ($now - $started).TotalSeconds -ge $MaxSeconds) {
        Write-FrameLog ("VERDICT: time-limit frame={0}" -f $lastFrame)
        break
    }
    if ($null -ne $win -and -not (Get-Process -Id $win.Pid -ErrorAction SilentlyContinue)) {
        Write-FrameLog ("VERDICT: process-gone frame={0}" -f $lastFrame)
        break
    }
    Start-Sleep -Milliseconds $PollMs
}

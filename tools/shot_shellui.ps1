param([string]$Out = '_TempData/shot.png', [int]$WaitSeconds = 110)
# Capture the kyty_emulator window via Win32 BitBlt.
Add-Type @'
using System;
using System.Collections.Generic;
using System.Runtime.InteropServices;
using System.Text;

public class WinCap {
    [DllImport("user32.dll")]
    public static extern bool GetWindowRect(IntPtr hWnd, out RECT lpRect);
    [DllImport("user32.dll")]
    public static extern bool SetForegroundWindow(IntPtr hWnd);
    [DllImport("user32.dll")]
    public static extern bool ShowWindow(IntPtr hWnd, int nCmdShow);
    [DllImport("user32.dll")]
    public static extern bool IsWindowVisible(IntPtr hWnd);
    [DllImport("user32.dll")]
    public static extern uint GetWindowThreadProcessId(IntPtr hWnd, out uint pid);
    [DllImport("user32.dll", CharSet = CharSet.Unicode)]
    public static extern int GetWindowText(IntPtr hWnd, StringBuilder text, int maxCount);

    private delegate bool EnumWindowsProc(IntPtr hWnd, IntPtr lParam);
    [DllImport("user32.dll")]
    private static extern bool EnumWindows(EnumWindowsProc cb, IntPtr lParam);

    public struct RECT { public int Left; public int Top; public int Right; public int Bottom; }

    public static IntPtr FindEmuWindow(uint targetPid) {
        IntPtr found = IntPtr.Zero;
        int maxArea = 0;
        EnumWindows((h, p) => {
            uint pid;
            GetWindowThreadProcessId(h, out pid);
            if (targetPid != 0 && pid != targetPid) return true;
            if (!IsWindowVisible(h)) return true;
            RECT r;
            if (GetWindowRect(h, out r)) {
                int area = (r.Right - r.Left) * (r.Bottom - r.Top);
                if (area > maxArea && (r.Right - r.Left) > 200) {
                    maxArea = area;
                    found = h;
                }
            }
            return true;
        }, IntPtr.Zero);
        return found;
    }
}
'@

$sw = [Diagnostics.Stopwatch]::StartNew()
$hwnd = [IntPtr]::Zero
while ($sw.Elapsed.TotalSeconds -lt $WaitSeconds) {
    $p = Get-Process kyty_emulator -ErrorAction SilentlyContinue | Select-Object -First 1
    if ($p) {
        $h = [WinCap]::FindEmuWindow($p.Id)
        if ($h -ne [IntPtr]::Zero) { $hwnd = $h; break }
    }
    Start-Sleep -Seconds 2
}
if ($hwnd -eq [IntPtr]::Zero) { Write-Output 'SHOT-NO-WINDOW'; exit 1 }

$remain = $WaitSeconds - $sw.Elapsed.TotalSeconds
if ($remain -gt 0) { Start-Sleep -Seconds ([int]$remain) }

Add-Type -AssemblyName System.Drawing
[WinCap]::ShowWindow($hwnd, 9) | Out-Null
[WinCap]::SetForegroundWindow($hwnd) | Out-Null
Start-Sleep -Milliseconds 500

$rc = New-Object WinCap+RECT
[WinCap]::GetWindowRect($hwnd, [ref]$rc) | Out-Null
$w = $rc.Right - $rc.Left; $h = $rc.Bottom - $rc.Top
if ($w -le 0 -or $h -le 0) { Write-Output 'SHOT-ZERO-RECT'; exit 1 }

$bmp = New-Object Drawing.Bitmap $w, $h
$gfx = [Drawing.Graphics]::FromImage($bmp)
$gfx.CopyFromScreen($rc.Left, $rc.Top, 0, 0, (New-Object Drawing.Size $w, $h), [Drawing.CopyPixelOperation]::SourceCopy)
$gfx.Dispose()
$bmp.Save($Out, [Drawing.Imaging.ImageFormat]::Png)
$bmp.Dispose()
Write-Output "$Out captured ($w x $h)"

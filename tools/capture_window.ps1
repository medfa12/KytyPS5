param(
    [Parameter(Mandatory = $true)][int]$ProcessId,
    [Parameter(Mandatory = $true)][string]$Out
)
$ErrorActionPreference = 'Stop'
Add-Type -AssemblyName System.Drawing
Add-Type @'
using System;
using System.Runtime.InteropServices;
public static class WindowReadback {
    [DllImport("user32.dll")] public static extern bool PrintWindow(IntPtr h, IntPtr dc, uint flags);
    [DllImport("user32.dll")] public static extern bool GetWindowRect(IntPtr h, out Rect r);
    [DllImport("user32.dll")] public static extern IntPtr SetThreadDpiAwarenessContext(IntPtr context);
    public static bool GetPhysicalWindowRect(IntPtr h, out Rect r) {
        // PrintWindow supplies physical pixels. Match its bounds even when the
        // PowerShell host is DPI-unaware; virtualized bounds crop the readback.
        IntPtr previous = SetThreadDpiAwarenessContext(new IntPtr(-4));
        try { return GetWindowRect(h, out r); }
        finally { if (previous != IntPtr.Zero) SetThreadDpiAwarenessContext(previous); }
    }
    public struct Rect { public int L,T,R,B; }
}
'@
$process = Get-Process -Id $ProcessId
$window = $process.MainWindowHandle
if ($window -eq [IntPtr]::Zero) { throw 'Target has no main window' }
$rect = New-Object WindowReadback+Rect
if (-not [WindowReadback]::GetPhysicalWindowRect($window, [ref]$rect)) { throw 'GetWindowRect failed' }
$bitmap = New-Object Drawing.Bitmap ($rect.R - $rect.L), ($rect.B - $rect.T)
try {
    $graphics = [Drawing.Graphics]::FromImage($bitmap)
    try {
        $dc = $graphics.GetHdc()
        try {
            if (-not [WindowReadback]::PrintWindow($window, $dc, 2)) { throw 'PrintWindow failed' }
        } finally { $graphics.ReleaseHdc($dc) }
    } finally { $graphics.Dispose() }
    $bitmap.Save([IO.Path]::GetFullPath($Out))
} finally { $bitmap.Dispose() }
Write-Output $process.MainWindowTitle
Write-Output "Captured $Out"

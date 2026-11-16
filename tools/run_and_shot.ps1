param(
    [string]$LogName = 'efboot18',
    [int]$Seconds = 120,
    [int]$ShotAt = 95,
    [string]$OutShot = 'emu_efboot18.png'
)

$runJob = Start-Job -ScriptBlock {
    param($log, $sec)
    Set-Location $using:PWD
    powershell -NoProfile -ExecutionPolicy Bypass -File tools/run_shellui_1360.ps1 -LogName $log -RnpsReplay -OfflineDatabases -Seconds $sec -IdleSeconds 0 -Visible -FrameDump
} -ArgumentList $LogName, $Seconds

Write-Output "Waiting $ShotAt seconds for ShellUI to reach render loop..."
Start-Sleep -Seconds $ShotAt

Write-Output "Capturing screenshot..."
powershell -NoProfile -ExecutionPolicy Bypass -File tools/shot_shellui.ps1 -Out $OutShot -WaitSeconds 20

Write-Output "Waiting for run to complete..."
Wait-Job $runJob | Receive-Job
Remove-Job $runJob

# Convert any dumped PPM frames to PNG
$dumpDir = "_Runs/$LogName/frames"
if (Test-Path $dumpDir) {
    Write-Output "Checking GPU frame dumps in $dumpDir..."
    python -c "
import glob, os
from PIL import Image

dump_dir = '$dumpDir'
ppms = sorted(glob.glob(os.path.join(dump_dir, '*.ppm')))
print(f'Found {len(ppms)} dumped GPU frames.')
for ppm in ppms:
    png = ppm.replace('.ppm', '.png')
    img = Image.open(ppm)
    img.save(png)
    print(f'Converted {ppm} ({img.size}) -> {png}')
"
}

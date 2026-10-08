<#
.SYNOPSIS
    Flashes a bot's merged factory image from scripts/rdytoflashbin/<bot>/.

.DESCRIPTION
    Asks which bot (default simplebot). Chip is read from build_info.json written by
    buildbinesp32.ps1. COM port is auto-detected if only one exists.

.EXAMPLE
    .\flashbinesp32.ps1
    .\flashbinesp32.ps1 -Bot rfbot -Port COM9
#>
[CmdletBinding()]
param(
    [string]$Bot,
    [string]$Port,
    [int]$Baud = 921600
)

$ErrorActionPreference = "Stop"
. (Join-Path $PSScriptRoot "_common.ps1")

Write-Host "========================================================" -ForegroundColor Cyan
Write-Host "   Robot Fleet ESP32 Flashing Tool" -ForegroundColor Cyan
Write-Host "========================================================" -ForegroundColor Cyan

$Bot = Select-Option "Which bot?" $Bots $Bot

$BinDir  = Join-Path $BinRoot $Bot
$Factory = Join-Path $BinDir "factory.bin"
$Info    = Join-Path $BinDir "build_info.json"
if (-not (Test-Path $Factory)) { throw "No build found at $Factory. Run .\buildbinesp32.ps1 first." }
$meta = Get-Content $Info -Raw | ConvertFrom-Json

if (-not $Port) {
    $ports = @([System.IO.Ports.SerialPort]::GetPortNames())
    if ($ports.Count -eq 1) {
        $Port = $ports[0]
        Write-Host "-> Auto-detected port: $Port" -ForegroundColor Green
    } else {
        $hint = if ($ports.Count) { $ports -join ', ' } else { "none detected" }
        $Port = Read-Host "COM port ($hint)"
    }
}
if (-not $Port) { throw "No COM port specified." }
$Port = $Port.ToUpper().Trim()

Write-Host "--> Flashing $($meta.bot)$($meta.device).local ($($meta.chip)) on $Port" -ForegroundColor Magenta

Enable-Idf
python -m esptool --chip $meta.chip -p $Port -b $Baud write_flash 0x0 $Factory
if ($LASTEXITCODE -ne 0) { throw "Flash failed with exit code $LASTEXITCODE" }

Write-Host "`n[SUCCESS] Device should appear at http://$($meta.bot)$($meta.device).local/" -ForegroundColor Green

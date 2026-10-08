<#
.SYNOPSIS
    Interactive build for any robot-fleet bot. Outputs binaries to scripts/rdytoflashbin/<bot>/.

.DESCRIPTION
    Asks (Enter = default) for:
      1. Chip         (default esp32c3)
      2. Bot          (default simplebot)
      3. mDNS number  (default 1)  -> passed as -DDEVICE_NUMBER=<N>  => <bot><N>.local
    Any value passed as a parameter is not asked. Optionally flashes right after with -Port.

.EXAMPLE
    .\buildbinesp32.ps1
    .\buildbinesp32.ps1 -Chip esp32s3 -Bot cambot -DeviceNumber 2
    .\buildbinesp32.ps1 -Port COM9     # build, then flash
#>
[CmdletBinding()]
param(
    [string]$Chip,
    [string]$Bot,
    [int]$DeviceNumber = -1,
    [string]$Port,
    [switch]$Clean
)

$ErrorActionPreference = "Stop"
. (Join-Path $PSScriptRoot "_common.ps1")

Write-Host "========================================================" -ForegroundColor Cyan
Write-Host "   Robot Fleet ESP32 Firmware Build" -ForegroundColor Cyan
Write-Host "========================================================" -ForegroundColor Cyan

$Chip         = Select-Option "Which ESP32?" $Chips $Chip
$Bot          = Select-Option "Which bot?"   $Bots  $Bot
$DeviceNumber = Select-DeviceNumber $DeviceNumber

$ProjectDir = Join-Path $PlatformsDir $Bot
$OutputDir  = Join-Path $BinRoot $Bot
if (-not (Test-Path $ProjectDir)) { throw "Project not found: $ProjectDir" }

Write-Host ""
Write-Host "--> $Bot | $Chip | $Bot$DeviceNumber.local" -ForegroundColor Magenta

Enable-Idf

Push-Location $ProjectDir
try {
    # Switch target if sdkconfig is missing or for a different chip
    $needTarget = $true
    if (Test-Path "sdkconfig") {
        $needTarget = -not (Select-String -Path "sdkconfig" -Pattern "^CONFIG_IDF_TARGET=`"$Chip`"" -Quiet)
    }
    if ($needTarget) {
        Write-Host "-> Setting target $Chip..." -ForegroundColor Yellow
        idf.py "-DDEVICE_NUMBER=$DeviceNumber" set-target $Chip
        if ($LASTEXITCODE -ne 0) { throw "set-target failed" }
    } elseif ($Clean) {
        idf.py fullclean
    }

    Write-Host "-> Building (DEVICE_NUMBER=$DeviceNumber)..." -ForegroundColor Green
    idf.py "-DDEVICE_NUMBER=$DeviceNumber" build
    if ($LASTEXITCODE -ne 0) { throw "Build failed with exit code $LASTEXITCODE" }

    if (Test-Path $OutputDir) { Remove-Item "$OutputDir\*" -Force -Recurse }
    New-Item -ItemType Directory -Path $OutputDir -Force | Out-Null

    # Merge all images into one factory bin using IDF's own offsets (works for any partition layout)
    Write-Host "-> Generating merged factory image..." -ForegroundColor Green
    Push-Location "build"
    try {
        python -m esptool --chip $Chip merge_bin -o (Join-Path $OutputDir "factory.bin") "@flash_args"
        if ($LASTEXITCODE -ne 0) { throw "merge_bin failed" }
    } finally { Pop-Location }

    @{ bot = $Bot; chip = $Chip; device = $DeviceNumber; built = (Get-Date).ToString("s") } |
        ConvertTo-Json | Set-Content (Join-Path $OutputDir "build_info.json")

    Write-Host "`n[SUCCESS] $Bot$DeviceNumber.local ($Chip) ready in: $OutputDir" -ForegroundColor Green
}
finally {
    Pop-Location
}

if ($Port) {
    & (Join-Path $PSScriptRoot "flashbinesp32.ps1") -Bot $Bot -Port $Port
} else {
    Write-Host "Flash with:  .\flashbinesp32.ps1 -Bot $Bot" -ForegroundColor Yellow
}

# Shared helpers for buildbinesp32.ps1 / flashbinesp32.ps1 (dot-sourced).

$RepoRoot     = Split-Path -Parent $PSScriptRoot
$PlatformsDir = Join-Path $RepoRoot "firmware\platforms"
$BinRoot      = Join-Path $PSScriptRoot "rdytoflashbin"

$Chips = @("esp32c3", "esp32s3")   # first = default
$Bots  = @("simplebot", "cambot", "carbot", "dogbot_v1", "mybot", "rfbot", "speakerbot")  # first = default

function Select-Option {
    param([string]$Title, [string[]]$Options, [string]$Preset)
    if ($Preset) {
        if ($Options -notcontains $Preset) { throw "Invalid value '$Preset'. Choose from: $($Options -join ', ')" }
        return $Preset
    }
    Write-Host ""
    Write-Host $Title -ForegroundColor Cyan
    for ($i = 0; $i -lt $Options.Count; $i++) {
        $tag = if ($i -eq 0) { "  (default)" } else { "" }
        Write-Host ("  {0}) {1}{2}" -f ($i + 1), $Options[$i], $tag)
    }
    while ($true) {
        $ans = Read-Host "Select [1-$($Options.Count), Enter = $($Options[0])]"
        if ([string]::IsNullOrWhiteSpace($ans)) { return $Options[0] }
        $n = 0
        if ([int]::TryParse($ans, [ref]$n) -and $n -ge 1 -and $n -le $Options.Count) { return $Options[$n - 1] }
        if ($Options -contains $ans.Trim()) { return $ans.Trim() }
        Write-Host "  Invalid choice, try again." -ForegroundColor Red
    }
}

function Select-DeviceNumber {
    param([int]$Preset)
    if ($Preset -ge 1) { return $Preset }
    Write-Host ""
    while ($true) {
        $ans = Read-Host "mDNS device number [Enter = 1]"
        if ([string]::IsNullOrWhiteSpace($ans)) { return 1 }
        $n = 0
        if ([int]::TryParse($ans, [ref]$n) -and $n -ge 1) { return $n }
        Write-Host "  Enter a positive integer." -ForegroundColor Red
    }
}

function Enable-Idf {
    $candidates = @(
        "C:\Espressif\tools\Microsoft.v5.5.4.PowerShell_profile.ps1",
        "C:\Espressif\tools\Microsoft.v6.0.PowerShell_profile.ps1"
    )
    $idfProfile = $candidates | Where-Object { Test-Path $_ } | Select-Object -First 1
    if (-not $idfProfile) { throw "ESP-IDF activation script not found. Checked: $($candidates -join ', ')" }
    Write-Host "-> Activating ESP-IDF: $idfProfile" -ForegroundColor Green
    $env:PYTHONIOENCODING = "utf-8"
    $env:PYTHONUTF8 = "1"
    . $idfProfile
}

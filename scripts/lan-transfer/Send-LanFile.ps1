#Requires -Version 5.1
[CmdletBinding()]
param(
    [Parameter(Mandatory = $true)] [string] $Path,
    [Parameter(Mandatory = $true)] [string] $ComputerName,
    [ValidateRange(1, 65535)] [int] $Port = 45877,
    [string] $Name,
    [System.Security.SecureString] $Secret
)
. "$PSScriptRoot\LanFileTransfer.Common.ps1"
try {
    $source = (Get-Item -LiteralPath $Path -ErrorAction Stop)
    if ($source -isnot [System.IO.FileInfo]) { throw 'Path must refer to one file.' }
    if (-not $Name) { $Name = $source.Name }
    $plainSecret = Read-TransferSecret $Secret
    [LanTransferV1.Engine]::Send($source.FullName, $ComputerName, $Port, $plainSecret, $Name)
} catch {
    Write-Error $_ -ErrorAction Continue
    exit 1
} finally { $plainSecret = $null }

exit 0

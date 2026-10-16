#Requires -Version 5.1
[CmdletBinding()]
param(
    [Parameter(Mandatory = $true)] [string] $Destination,
    [string] $ListenAddress = '0.0.0.0',
    [ValidateRange(1, 65535)] [int] $Port = 45877,
    [System.Security.SecureString] $Secret,
    [switch] $Once
)
. "$PSScriptRoot\LanFileTransfer.Common.ps1"
try {
    $target = $ExecutionContext.SessionState.Path.GetUnresolvedProviderPathFromPSPath($Destination)
    $plainSecret = Read-TransferSecret $Secret
    [LanTransferV1.Engine]::Receive($target, $ListenAddress, $Port, $plainSecret, $Once.IsPresent)
} catch {
    Write-Error $_ -ErrorAction Continue
    exit 1
} finally { $plainSecret = $null }

exit 0

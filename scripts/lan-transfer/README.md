# Windows LAN file transfer

Copy this entire folder to both Windows machines. Keep the common script next to the sender and receiver scripts. Requires Windows PowerShell 5.1 or PowerShell 7, with no modules to install.

Transfers use a fixed 1 MiB buffer and 64-bit file sizes, so files do not have to fit in RAM. Interrupted transfers resume when you rerun the same sender command. SHA-256 verification is required before a file is published under its final name.

## 1. Start the receiver

Open PowerShell in this folder on the destination machine:

~~~powershell
powershell.exe -NoProfile -ExecutionPolicy Bypass -File .\Receive-LanFile.ps1 -Destination 'D:\Received'
~~~

Enter a strong, unique shared secret of at least 16 characters at the hidden prompt. Use the same secret on the sender. The destination folder is created if needed. The receiver stays running and accepts one transfer at a time.

Find the receiver's local IPv4 address with ipconfig, for example 192.168.1.50. Use the address of the network adapter connected to the same LAN as the sender.

If Windows Firewall blocks the connection, run this once in an administrator PowerShell window on the receiver:

~~~powershell
New-NetFirewallRule -DisplayName 'LAN File Transfer 45877' -Direction Inbound -Action Allow -Protocol TCP -LocalPort 45877 -Profile Private -RemoteAddress LocalSubnet
~~~

This rule applies to a Private network profile. Use it on a network you trust. The scripts do not change firewall settings automatically. Remove the rule when no longer needed:

~~~powershell
Remove-NetFirewallRule -DisplayName 'LAN File Transfer 45877'
~~~

## 2. Send a file

Open PowerShell in this folder on the source machine, substituting the receiver's actual IP address:

~~~powershell
powershell.exe -NoProfile -ExecutionPolicy Bypass -File .\Send-LanFile.ps1 -Path 'D:\Videos\large-video.mkv' -ComputerName 192.168.1.50
~~~

Enter the same shared secret. The sender first hashes the source file, then transfers it and waits for the receiver to verify it. Initial hashing and final verification can take time for very large files. Transfer progress and throughput are printed about every two seconds.

The ExecutionPolicy option above applies only to the launched PowerShell process. It does not change the machine's execution policy or override organizational policy.

## Options

- Both scripts: -Port 45877 selects the TCP port. Use the same port on both machines and in the firewall rule.
- Receiver: -ListenAddress 192.168.1.50 binds to a specific local interface. The default, 0.0.0.0, listens on all IPv4 interfaces.
- Receiver: -Once exits after one connection, with a failure exit code if the transfer fails.
- Sender: -Name 'new-name.mkv' chooses a different destination filename. Subdirectories are not accepted.
- Both scripts: -Secret accepts a SecureString for automation. Omitting it displays a hidden prompt.

To send several files without retyping the secret, use a PowerShell session where script execution is permitted:

~~~powershell
$secret = Read-Host 'Shared secret' -AsSecureString
Get-ChildItem -LiteralPath 'D:\Videos' -File | ForEach-Object {
    & .\Send-LanFile.ps1 -Path $_.FullName -ComputerName 192.168.1.50 -Secret $secret
    if ($LASTEXITCODE -eq 1) { throw 'Transfer failed. Rerun the failed file to resume.' }
}
~~~

Files are sent individually. Folder trees, timestamps, permissions, and alternate data streams are not copied.

## Resuming and reliability

If a connection drops, close or wait for the failed sender, ensure the receiver is running, and rerun the same sender command. Keep the same source contents, destination folder, and destination name. Partial data lives in the destination's .lan-transfer subfolder and survives receiver restarts. The source is locked against writes while it is being sent.

Existing destination files are never overwritten. An existing file with the same contents is verified and reported as already received. For different contents, choose another -Name or move the old file yourself.

A checksum mismatch removes the affected partial file and reports failure. Rerun the sender to start that file again. If the source changes, its old partial file is no longer reused. You can remove abandoned .part files from .lan-transfer while the receiver is stopped.

Network reads and writes time out after five minutes without progress. Verification sends periodic heartbeat messages. The sender does not automatically retry. Close the receiver's PowerShell window to stop it; a partial transfer can be resumed later.

Use NTFS or exFAT for files larger than 4 GiB; FAT32 cannot store them. Ensure enough free space for the remaining bytes and keep both machines awake. Wired Ethernet is preferable for long transfers. Firewalls, guest Wi-Fi isolation, disconnected VPN routes, or insufficient disk space can still interrupt a transfer.

## Security

Use only on a trusted LAN. Challenge-response authentication uses HMAC-SHA-256 and does not send the shared secret directly. File contents and metadata are unencrypted, and this protocol does not protect against an active attacker on the network. Do not expose the port to the internet. For untrusted networks, use an encrypted transfer tool or an encrypted VPN.

## Validation

Tested locally with Windows PowerShell 5.1: a 32 MiB transfer, byte-for-byte comparison, resuming after a connection ended at 3 MiB, empty files, duplicate delivery, checksum failure, wrong secrets, and existing-file protection. Actual throughput and multi-gigabyte transfers across two physical machines depend on the network and disks.

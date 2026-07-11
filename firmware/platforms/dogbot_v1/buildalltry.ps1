$results = @()
$startTime = Get-Date

for ($i = 8; $i -ge 0; $i--) {
    Write-Host "`n[$(Get-Date -Format 'HH:mm:ss')] Building for device $i..." -ForegroundColor Cyan
    Write-Host "Folder will be: dogbot$i\" -ForegroundColor Gray
    
    $buildStart = Get-Date
    
    # Run the build
    $output = idf.py -D BOARD=esp32c3_dog -D DEVICE_NUMBER=$i build 2>&1
    $success = $LASTEXITCODE -eq 0
    
    $buildEnd = Get-Date
    $duration = ($buildEnd - $buildStart).TotalSeconds
    
    if ($success) {
        Write-Host "û Build successful (took $([math]::Round($duration, 1))s)" -ForegroundColor Green
        
        $folderName = "dogbot$i"
        New-Item -ItemType Directory -Force -Path $folderName | Out-Null
        
        if (Test-Path "build\mojDogv1.bin") {
            Copy-Item "build\mojDogv1.bin" -Destination "$folderName\mojDogv1.bin" -Force
            $fileSize = (Get-Item "$folderName\mojDogv1.bin").Length
            Write-Host "û Copied to $folderName\mojDogv1.bin ($([math]::Round($fileSize/1KB, 1)) KB)" -ForegroundColor Green
        } else {
            Write-Host "? Warning: mojDogv1.bin not found in build directory" -ForegroundColor Yellow
        }
        
        $results += [PSCustomObject]@{
            Device = $i
            Status = "Success"
            Duration = "$([math]::Round($duration, 1))s"
            Folder = $folderName
        }
    } else {
        Write-Host "? Build failed for device $i" -ForegroundColor Red
        $results += [PSCustomObject]@{
            Device = $i
            Status = "Failed"
            Duration = "$([math]::Round($duration, 1))s"
            Folder = "N/A"
        }
    }
}

$totalTime = (Get-Date) - $startTime

Write-Host "`n" -NoNewline
Write-Host "=== BUILD SUMMARY ===" -ForegroundColor Yellow
$results | Format-Table -AutoSize
Write-Host "Total time: $([math]::Round($totalTime.TotalMinutes, 1)) minutes" -ForegroundColor Yellow

$successCount = ($results | Where-Object { $_.Status -eq "Success" }).Count
$failCount = ($results | Where-Object { $_.Status -eq "Failed" }).Count
Write-Host "Successful: $successCount, Failed: $failCount" -ForegroundColor $(if ($failCount -eq 0) { "Green" } else { "Red" })


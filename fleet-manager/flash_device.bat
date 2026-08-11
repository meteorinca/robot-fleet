@echo off
setlocal EnableExtensions EnableDelayedExpansion

REM ========================================================
REM Helper script to flash pre-built device binaries to ESP32-C3
REM Usage: flash_device.bat <DEVICE_NUMBER> [COM_PORT]
REM Example: flash_device.bat 12
REM Example: flash_device.bat 12 COM4
REM ========================================================

if "%~1"=="" goto :usage

set "DEV_NUM=%~1"
set "PORT=%~2"

set "SCRIPT_DIR=%~dp0"
set "BIN_DIR=%SCRIPT_DIR%device_bins\device_%DEV_NUM%"
set "FIRMWARE_BUILD=%SCRIPT_DIR%..\firmware\platforms\dogbot_v1\build"

REM Check device folder exists
if not exist "%BIN_DIR%" goto :no_bin_dir
goto :bin_dir_ok

:no_bin_dir
echo ERROR: Device folder not found at:
echo %BIN_DIR%
echo.
echo Please run createallbinsYo.bat first to generate the binary files.
echo.
goto :usage

:bin_dir_ok
REM --------------------------------------------------------
REM Auto-detect COM port if not specified (skips COM1)
REM --------------------------------------------------------
if not "%PORT%"=="" goto :port_set

echo Detecting connected ESP32 COM port...

set "PORT_FILE=%TEMP%\esp_port_%RANDOM%.txt"

powershell -NoProfile -Command "$ErrorActionPreference='SilentlyContinue'; $ports = @(); $devs = Get-PnpDevice -Class Ports -FriendlyName '*USB*Serial*'; foreach ($dev in $devs) { if ($dev.FriendlyName -match 'COM(\d+)') { $n = [int]$matches[1]; if ($n -ne 1) { $ports += $n } } }; if ($ports.Count -eq 0) { 'NONE' } else { 'COM' + ($ports | Sort-Object -Descending | Select-Object -First 1) }" > "%PORT_FILE%"

set /p "PORT=" < "%PORT_FILE%"
del "%PORT_FILE%" 2>nul

if "%PORT%"=="NONE" (
    echo.
    echo ERROR: No USB Serial ports found.
    echo Please plug in your ESP32 or specify the port manually:
    echo   flash_device.bat %DEV_NUM% COM21
    echo.
    exit /b 1
)

echo Found port: %PORT%
echo.

:port_set

if not defined PORT (
    echo.
    echo ERROR: No active ESP32 board detected on USB.
    echo Please plug in your ESP32 board and try again, or specify the port manually:
    echo   flash_device.bat %DEV_NUM% COM3
    echo.
    exit /b 1
)

echo ========================================================
echo Flashing Device %DEV_NUM% on %PORT% (ESP32-C3)
echo ========================================================
echo Device Folder: %BIN_DIR%
echo Target Port: %PORT%
echo.

REM --------------------------------------------------------
REM Locate main application binary dynamically
REM --------------------------------------------------------
set "APP_BIN="

for %%F in ("%BIN_DIR%\*.bin") do (
    if /I not "%%~nxF"=="bootloader.bin" (
        if /I not "%%~nxF"=="partition-table.bin" (
            if /I not "%%~nxF"=="ota_data_initial.bin" (
                set "APP_BIN=%%~nxF"
            )
        )
    )
)

if not defined APP_BIN (
    echo ERROR: Could not locate the main application .bin file in:
    echo %BIN_DIR%
    exit /b 1
)

echo Found App Binary: %APP_BIN%

REM --------------------------------------------------------
REM Locate bootloader, partition table, and OTA data
REM --------------------------------------------------------
set "BOOTLOADER_BIN="
if exist "%BIN_DIR%\bootloader.bin" (
    set "BOOTLOADER_BIN=%BIN_DIR%\bootloader.bin"
)
if not defined BOOTLOADER_BIN (
    if exist "%FIRMWARE_BUILD%\bootloader\bootloader.bin" (
        set "BOOTLOADER_BIN=%FIRMWARE_BUILD%\bootloader\bootloader.bin"
    )
)

set "PARTITION_BIN="
if exist "%BIN_DIR%\partition-table.bin" (
    set "PARTITION_BIN=%BIN_DIR%\partition-table.bin"
)
if not defined PARTITION_BIN (
    if exist "%FIRMWARE_BUILD%\partition_table\partition-table.bin" (
        set "PARTITION_BIN=%FIRMWARE_BUILD%\partition_table\partition-table.bin"
    )
)

set "OTADATA_BIN="
if exist "%BIN_DIR%\ota_data_initial.bin" (
    set "OTADATA_BIN=%BIN_DIR%\ota_data_initial.bin"
)
if not defined OTADATA_BIN (
    if exist "%FIRMWARE_BUILD%\ota_data_initial.bin" (
        set "OTADATA_BIN=%FIRMWARE_BUILD%\ota_data_initial.bin"
    )
)

if not defined BOOTLOADER_BIN (
    echo ERROR: Could not locate bootloader.bin in:
    echo %BIN_DIR%
    echo or:
    echo %FIRMWARE_BUILD%\bootloader\
    exit /b 1
)

if not defined PARTITION_BIN (
    echo ERROR: Could not locate partition-table.bin in:
    echo %BIN_DIR%
    echo or:
    echo %FIRMWARE_BUILD%\partition_table\
    exit /b 1
)

REM --------------------------------------------------------
REM Build esptool command-line arguments
REM --------------------------------------------------------
set "FLASH_ARGS=0x0 "%BOOTLOADER_BIN%" 0x8000 "%PARTITION_BIN%""

if defined OTADATA_BIN (
    set "FLASH_ARGS=!FLASH_ARGS! 0xe000 "%OTADATA_BIN%""
)

set "FLASH_ARGS=!FLASH_ARGS! 0x20000 "%BIN_DIR%\%APP_BIN%""

echo.
echo Executing esptool.exe...
echo.

esptool.exe --chip esp32c3 -p %PORT% -b 460800 write_flash --flash_mode dio --flash_size 4MB !FLASH_ARGS!

if errorlevel 1 (
    echo.
    echo ========================================================
    echo ERROR: Flashing failed on %PORT%.
    echo Please verify the USB connection, COM port, and board state.
    echo ========================================================
    exit /b 1
)

echo.
echo ========================================================
echo SUCCESS: Device %DEV_NUM% flashed successfully on %PORT%!
echo ========================================================

endlocal
exit /b 0

:usage
echo ========================================================
echo Flash Device Helper Script (ESP32-C3)
echo ========================================================
echo Usage: flash_device.bat ^<DEVICE_NUMBER^> [COM_PORT]
echo.
echo Examples:
echo   flash_device.bat 12
echo   flash_device.bat 12 COM3
echo ========================================================

if exist "%~dp0device_bins" (
    echo.
    echo Available device folders in device_bins:
    dir /b /ad "%~dp0device_bins\device_*" 2>nul
)

exit /b 1
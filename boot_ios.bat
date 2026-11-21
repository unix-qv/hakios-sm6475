@echo off
title HakiOS Auto-Booter for POCO M8 5G
setlocal enabledelayedexpansion

echo ====================================================
echo             HakiOS Auto-Boot for POCO M8 5G
echo ====================================================
echo.

set "SCRIPT_DIR=%~dp0"
set "PATH=%SCRIPT_DIR%bin\windows;%PATH%"

set "FASTBOOT=fastboot.exe"
if exist "%SCRIPT_DIR%bin\windows\fastboot.exe" (
    set "FASTBOOT=%SCRIPT_DIR%bin\windows\fastboot.exe"
)

set "IMG=%SCRIPT_DIR%images\xnu-usb.img"
set "PAYLOAD=%SCRIPT_DIR%images\run-payload.bin"
set "APFS=%SCRIPT_DIR%images\system.apfs"

if not exist "!IMG!" (
    echo [!] Error: images\xnu-usb.img not found.
    pause
    exit /b 1
)

if not exist "!PAYLOAD!" (
    echo [!] Error: images\run-payload.bin not found.
    pause
    exit /b 1
)

if not exist "!APFS!" (
    echo [!] Error: images\system.apfs not found.
    pause
    exit /b 1
)

echo [*] Waiting for device in fastboot mode...
:wait_fastboot
"!FASTBOOT!" devices 2>nul | findstr /i "fastboot" >nul
if errorlevel 1 (
    timeout /t 1 /nobreak >nul
    goto wait_fastboot
)

echo [+] Device detected!

:: Check if product is xnuloader
set "IS_XNU="
for /f "tokens=*" %%a in ('"!FASTBOOT!" getvar product 2^>^&1') do (
    echo %%a | findstr /i "xnuloader" >nul
    if not errorlevel 1 set "IS_XNU=1"
)

if defined IS_XNU goto load_apfs

echo [*] Device in stock fastboot. Booting U-Boot loader (images\xnu-usb.img)...
"!FASTBOOT!" boot "!IMG!"
echo [*] Waiting for U-Boot loader to initialize...
timeout /t 4 /nobreak >nul

:wait_loader
"!FASTBOOT!" devices 2>nul | findstr /i "fastboot" >nul
if errorlevel 1 (
    timeout /t 1 /nobreak >nul
    goto wait_loader
)

:load_apfs
echo [*] Loading resident APFS system image into RAM (images\system.apfs)...
echo     Please wait, uploading ~2.4 GB to device memory...
"!FASTBOOT!" oem ramdisk
"!FASTBOOT!" stage "!APFS!"

echo [*] Staging iOS kernel payload (images\run-payload.bin)...
"!FASTBOOT!" stage "!PAYLOAD!"

echo [*] Booting iOS...
"!FASTBOOT!" oem run

echo [+] Boot command sent! iOS is starting on device.
echo.
pause
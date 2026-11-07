# Tao anh SPIFFS tu thu muc data\ va nap vao partition 'storage' (0x110000, 0x2E0000).
# Cach dung:
#   powershell -ExecutionPolicy Bypass -File tools\upload_fs.ps1 -Port COM3
# Truoc khi chay: dong Serial Monitor / Hercules / platformio device monitor dang giu cong COM.
param(
    [string]$Port = "COM3",
    [string]$DataDir = "data"
)

$ErrorActionPreference = "Stop"

$root    = Split-Path -Parent $PSScriptRoot
$pio     = Join-Path $env:USERPROFILE ".platformio"
$idf     = Join-Path $pio "packages\framework-espidf"
$python  = Join-Path $pio "penv\Scripts\python.exe"
$esptool = Join-Path $pio "packages\tool-esptoolpy\esptool.py"

# Phai khop voi partitions.csv
$offset = 0x1D0000
$size   = 0x220000

$srcDir = Join-Path $root $DataDir
$image  = Join-Path $root ".pio\spiffs_storage.bin"

if (-not (Test-Path $srcDir)) { throw "Khong thay thu muc '$srcDir'" }
if (-not (Test-Path $python)) { throw "Khong thay python cua PlatformIO: $python" }
if (-not (Test-Path $esptool)) { throw "Khong thay esptool: $esptool" }

Write-Host "==> Tao anh SPIFFS ($size byte) tu $srcDir"
& $python (Join-Path $idf "components\spiffs\spiffsgen.py") `
    --page-size 256 --block-size 4096 --obj-name-len 32 `
    $size $srcDir $image
if ($LASTEXITCODE -ne 0) { throw "spiffsgen.py that bai" }

Write-Host "==> Nap $image vao $Port @ 0x$($offset.ToString('X'))"
& $python $esptool --chip esp32 --port $Port --baud 460800 `
    --before default_reset --after hard_reset `
    write_flash $offset $image
if ($LASTEXITCODE -ne 0) { throw "esptool write_flash that bai" }

Write-Host "==> Xong. Mo lai Serial Monitor de xem log giai ma."

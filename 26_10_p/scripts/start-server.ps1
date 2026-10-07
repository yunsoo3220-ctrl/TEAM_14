# 서버를 띄운다. 80 포트를 쓰므로 관리자 권한이 필요하다.
#
#   powershell -ExecutionPolicy Bypass -File scripts\start-server.ps1
#   powershell -ExecutionPolicy Bypass -File scripts\start-server.ps1 -Port 8080 -NoAdmin
#
# 서버 출력은 logs\server.log 에 쌓인다. 이미 떠 있으면 먼저 내린다.

param(
    [int]$Port       = 80,
    [string]$DbPass  = $(if ($env:SKU_DB_PASS) { $env:SKU_DB_PASS } else { '1234' }),
    [string]$Project = (Split-Path -Parent $PSScriptRoot),
    [switch]$NoAdmin,
    [switch]$Stop
)

$ErrorActionPreference = 'Continue'

$exe    = Join-Path $Project 'server.exe'
$logDir = Join-Path $Project 'logs'
$log    = Join-Path $logDir 'server.log'
$errLog = Join-Path $logDir 'server.err.log'

if (-not (Test-Path $logDir)) { New-Item -ItemType Directory -Path $logDir | Out-Null }

# 이미 떠 있으면 내린다.
$running = Get-Process server -ErrorAction SilentlyContinue
if ($running) {
    Write-Host "실행 중인 server.exe 중지 (PID $($running.Id))"
    try {
        Stop-Process -Id $running.Id -Force -ErrorAction Stop
        for ($i = 0; $i -lt 20 -and (Get-Process server -ErrorAction SilentlyContinue); $i++) {
            Start-Sleep -Milliseconds 300
        }
    } catch {
        Write-Host "중지 실패 - 관리자 권한으로 실행하세요." -ForegroundColor Red
        exit 1
    }
}
if ($Stop) { Write-Host '중지했습니다.'; exit 0 }

if (-not (Test-Path $exe)) {
    Write-Host "server.exe 가 없습니다. 먼저 make 로 빌드하세요." -ForegroundColor Red
    exit 1
}

# 80 포트는 관리자 권한이 필요하다.
if ($Port -lt 1024 -and -not $NoAdmin) {
    $me = New-Object Security.Principal.WindowsPrincipal(
              [Security.Principal.WindowsIdentity]::GetCurrent())
    if (-not $me.IsInRole([Security.Principal.WindowsBuiltInRole]::Administrator)) {
        Write-Host "$Port 포트는 관리자 권한이 필요합니다. 관리자 PowerShell 에서 실행하거나 -Port 8080 을 쓰세요." -ForegroundColor Red
        exit 1
    }
}

# libmariadb.dll 을 찾을 수 있게 한다.
$env:PATH        = 'C:\msys64\ucrt64\bin;' + $env:PATH
$env:SKU_DB_PASS = $DbPass

Start-Process -FilePath $exe `
              -ArgumentList '--port', $Port `
              -WorkingDirectory $Project `
              -WindowStyle Hidden `
              -RedirectStandardOutput $log `
              -RedirectStandardError $errLog | Out-Null

Start-Sleep -Seconds 3
$p = Get-Process server -ErrorAction SilentlyContinue
if ($p) {
    Write-Host "서버 실행 중 (PID $($p.Id), 포트 $Port)" -ForegroundColor Green
    Write-Host "로그: $errLog"
    exit 0
}

Write-Host '서버가 바로 종료되었습니다. 로그를 확인하세요:' -ForegroundColor Red
if (Test-Path $errLog) { Get-Content -LiteralPath $errLog -Tail 20 }
exit 1

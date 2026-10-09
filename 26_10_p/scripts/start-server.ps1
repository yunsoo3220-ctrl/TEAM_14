# 서버를 띄운다. 80 포트를 쓰므로 관리자 권한이 필요하다.
#
#   powershell -ExecutionPolicy Bypass -File scripts\start-server.ps1
#   powershell -ExecutionPolicy Bypass -File scripts\start-server.ps1 -Port 8080 -NoAdmin
#   ... -NoEmbed      딥러닝 임베딩 서비스 없이 (TF-IDF 만으로 추천)
#
# 서버 출력은 logs\server.log, 임베딩 서비스 출력은 logs\embed.err.log 에 쌓인다.
# 이미 떠 있으면 먼저 내린다.

param(
    [int]$Port       = 80,
    [string]$DbPass  = $(if ($env:SKU_DB_PASS) { $env:SKU_DB_PASS } else { '1234' }),
    [string]$Project = (Split-Path -Parent $PSScriptRoot),
    [switch]$NoAdmin,
    [switch]$NoEmbed,
    [switch]$Stop
)

$ErrorActionPreference = 'Continue'

$exe    = Join-Path $Project 'server.exe'
$logDir = Join-Path $Project 'logs'
$log    = Join-Path $logDir 'server.log'
$errLog = Join-Path $logDir 'server.err.log'

if (-not (Test-Path $logDir)) { New-Item -ItemType Directory -Path $logDir | Out-Null }

# 임베딩 서비스 (ml\embed_server.py). PID 파일로 우리가 띄운 프로세스만 내린다.
$embedPid = Join-Path $logDir 'embed.pid'
$venvPy   = Join-Path $Project 'ml\.venv\Scripts\python.exe'
if (Test-Path $embedPid) {
    $old = Get-Process -Id (Get-Content $embedPid) -ErrorAction SilentlyContinue
    if ($old) {
        Write-Host "실행 중인 임베딩 서비스 중지 (PID $($old.Id))"
        Stop-Process -Id $old.Id -Force -ErrorAction SilentlyContinue
    }
    Remove-Item $embedPid -ErrorAction SilentlyContinue
}

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

# 딥러닝 임베딩 서비스를 먼저 띄운다. 처음에는 모델(약 470MB)을 내려받느라 오래 걸리지만,
# 서버는 기다리지 않는다. 서비스가 준비되면 다음 추천 요청 때 임베딩을 넣어 다시 학습한다.
if (-not $NoEmbed) {
    if (Test-Path $venvPy) {
        $ep = Start-Process -FilePath $venvPy `
                            -ArgumentList (Join-Path $Project 'ml\embed_server.py') `
                            -WorkingDirectory $Project `
                            -WindowStyle Hidden -PassThru `
                            -RedirectStandardOutput (Join-Path $logDir 'embed.log') `
                            -RedirectStandardError (Join-Path $logDir 'embed.err.log')
        Set-Content -Path $embedPid -Value $ep.Id
        Write-Host "임베딩 서비스 시작 (PID $($ep.Id), 127.0.0.1:8001) - 로그: logs\embed.log"
    } else {
        Write-Host "ml\.venv 가 없어 임베딩 없이 TF-IDF 만으로 추천합니다 (README '딥러닝 임베딩' 참고)." -ForegroundColor Yellow
    }
}

# 회원가입 인증 메일 설정 (smtp.env)
. (Join-Path $PSScriptRoot 'smtp-env.ps1')
if (Import-SmtpEnv $Project) {
    Write-Host "인증 메일 발송: $env:SKU_SMTP_URL ($env:SKU_SMTP_USER)"
} elseif ($env:SKU_MAIL_DEV -eq '1') {
    Write-Host '메일 개발 모드: 인증 코드를 서버 로그에만 남깁니다.' -ForegroundColor Yellow
} else {
    Write-Host 'smtp.env 가 없어 회원가입 인증 메일을 보낼 수 없습니다 (smtp.env.example 참고).' -ForegroundColor Yellow
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

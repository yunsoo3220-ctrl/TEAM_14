# 서버켜기.bat / 서버끄기.bat 이 부르는 스크립트. 더블클릭 한 번으로 켜고 끄게 한다.
#   1. 관리자 권한이 없으면 UAC 창을 띄워 관리자로 다시 실행
#   2. DB 비밀번호는 db.env 에서 읽고, 없으면 한 번 물어보고 저장
#   3. hosts 에 www.sku14.com 이 없으면 등록
#   4. start-server.ps1 로 서버를 띄우고 브라우저를 연다
#
# 인자:
#   -Stop  떠 있는 서버를 내리기만 한다
param([switch]$Stop)

$Project = Split-Path -Parent $PSScriptRoot
$Url     = 'http://www.sku14.com'

# 끝나기 전에 창이 바로 닫히지 않게 기다린다
function Wait-Close { Read-Host "`n엔터를 누르면 창이 닫힙니다" | Out-Null }

# 1. 관리자 권한으로 다시 실행 (80 포트, hosts 파일, 떠 있는 서버 중지에 필요하다)
$me = New-Object Security.Principal.WindowsPrincipal([Security.Principal.WindowsIdentity]::GetCurrent())
if (-not $me.IsInRole([Security.Principal.WindowsBuiltInRole]::Administrator)) {
    $argList = "-NoProfile -ExecutionPolicy Bypass -File `"$PSCommandPath`""
    if ($Stop) { $argList += ' -Stop' }
    try {
        Start-Process powershell.exe -Verb RunAs -ArgumentList $argList -ErrorAction Stop
    } catch {
        Write-Host '관리자 권한을 허용해야 서버를 켜고 끌 수 있습니다.' -ForegroundColor Red
        Wait-Close
    }
    exit
}

$startServer = Join-Path $PSScriptRoot 'start-server.ps1'

if ($Stop) {
    & $startServer -Stop
    Wait-Close
    exit
}

# 2. DB 비밀번호 (db.env, git 에 올리지 않는다)
$dbEnv = Join-Path $Project 'db.env'
$dbPass = $null
if (Test-Path $dbEnv) {
    foreach ($line in Get-Content -LiteralPath $dbEnv -Encoding UTF8) {
        if ($line -match '^\s*SKU_DB_PASS\s*=(.*)$') { $dbPass = $Matches[1].Trim() }
    }
}
if (-not $dbPass) {
    $sec = Read-Host 'MySQL root 비밀번호를 입력하세요 (한 번만 묻고 db.env 에 저장합니다)' -AsSecureString
    $dbPass = [Runtime.InteropServices.Marshal]::PtrToStringAuto(
                  [Runtime.InteropServices.Marshal]::SecureStringToBSTR($sec))
    Set-Content -LiteralPath $dbEnv -Value "SKU_DB_PASS=$dbPass" -Encoding UTF8
}

# 3. www.sku14.com 을 이 PC 로 연결 (처음 한 번)
$hosts = Join-Path $env:SystemRoot 'System32\drivers\etc\hosts'
if (-not (Select-String -LiteralPath $hosts -Pattern 'www\.sku14\.com' -Quiet)) {
    & (Join-Path $PSScriptRoot 'setup-domain.ps1')
}

# 4. 서버 실행 → 브라우저 열기
& $startServer -DbPass $dbPass
if ($LASTEXITCODE -eq 0) {
    Start-Process $Url
    Write-Host "`n브라우저에서 $Url 을 열었습니다. 끌 때는 서버끄기.bat 을 실행하세요." -ForegroundColor Green
} elseif (Test-Path (Join-Path $Project 'logs\server.err.log')) {
    if (Select-String -LiteralPath (Join-Path $Project 'logs\server.err.log') -Pattern 'Access denied' -Quiet) {
        Remove-Item -LiteralPath $dbEnv -ErrorAction SilentlyContinue
        Write-Host "`nDB 비밀번호가 틀렸습니다. 다시 실행하면 비밀번호를 새로 묻습니다." -ForegroundColor Red
    }
}
Wait-Close

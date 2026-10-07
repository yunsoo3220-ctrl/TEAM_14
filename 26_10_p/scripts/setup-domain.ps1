# www.sku14.com 을 이 PC 로 연결한다.
#
# hosts 파일은 관리자 권한이 있어야 고칠 수 있다.
#   PowerShell 을 "관리자 권한으로 실행" 한 뒤:
#     powershell -ExecutionPolicy Bypass -File scripts\setup-domain.ps1
#
# 되돌리려면:
#     powershell -ExecutionPolicy Bypass -File scripts\setup-domain.ps1 -Remove

param(
    [string]$Ip     = '127.0.0.1',
    [string]$Domain = 'sku14.com',
    [switch]$Remove
)

$ErrorActionPreference = 'Stop'

$hostsPath = Join-Path $env:SystemRoot 'System32\drivers\etc\hosts'
$marker    = '# sku-contest-board'
$names     = @("www.$Domain", $Domain)

# 관리자 권한 확인
$me = New-Object Security.Principal.WindowsPrincipal([Security.Principal.WindowsIdentity]::GetCurrent())
if (-not $me.IsInRole([Security.Principal.WindowsBuiltInRole]::Administrator)) {
    Write-Host '관리자 권한이 필요합니다. PowerShell 을 관리자로 다시 열고 실행하세요.' -ForegroundColor Red
    exit 1
}

# 기존 내용에서 이 스크립트가 넣은 줄을 모두 걷어낸다.
$lines = @()
if (Test-Path $hostsPath) {
    $lines = Get-Content -LiteralPath $hostsPath -Encoding UTF8
}
$kept = $lines | Where-Object { $_ -notmatch [regex]::Escape($marker) }

if ($Remove) {
    Set-Content -LiteralPath $hostsPath -Value $kept -Encoding UTF8
    Write-Host "$Domain 항목을 hosts 에서 제거했습니다." -ForegroundColor Green
}
else {
    # 다른 곳에서 이미 같은 이름을 쓰고 있으면 알려만 주고 중복은 만들지 않는다.
    foreach ($n in $names) {
        $conflict = $kept | Where-Object { $_ -match "^\s*[^#]\S*\s+.*\b$([regex]::Escape($n))\b" }
        if ($conflict) {
            Write-Host "주의: hosts 에 $n 항목이 이미 있습니다 -> $conflict" -ForegroundColor Yellow
        }
    }

    $added = $names | ForEach-Object { "$Ip`t$_`t$marker" }
    Set-Content -LiteralPath $hostsPath -Value ($kept + $added) -Encoding UTF8

    Write-Host '다음 줄을 hosts 에 넣었습니다.' -ForegroundColor Green
    $added | ForEach-Object { Write-Host "  $_" }
}

# DNS 캐시를 비워 바로 반영되게 한다.
try { ipconfig /flushdns | Out-Null } catch { }

Write-Host ''
Write-Host '확인:' -ForegroundColor Cyan
foreach ($n in $names) {
    try {
        $r = [System.Net.Dns]::GetHostAddresses($n) | Select-Object -First 1
        Write-Host "  $n -> $r"
    }
    catch {
        Write-Host "  $n -> 확인 실패" -ForegroundColor Yellow
    }
}

if (-not $Remove) {
    Write-Host ''
    Write-Host '이제 80 포트로 서버를 띄우고 http://www.sku14.com 을 열면 됩니다.' -ForegroundColor Cyan
    Write-Host '  $env:SKU_DB_PASS="<비밀번호>"; .\server.exe'
}

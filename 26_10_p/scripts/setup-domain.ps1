# www.sku14.com 을 이 PC 로 연결한다.
#
# hosts 파일은 관리자 권한이 있어야 고칠 수 있다.
#   PowerShell 을 "관리자 권한으로 실행" 한 뒤:
#     powershell -ExecutionPolicy Bypass -File scripts\setup-domain.ps1
#
# 되돌리려면:
#     powershell -ExecutionPolicy Bypass -File scripts\setup-domain.ps1 -Remove

# -Ip     연결할 IP (기본 127.0.0.1 = 이 PC)
# -Domain 도메인 (www. 붙은 이름과 붙지 않은 이름 두 개를 함께 등록)
# -Remove 이 스크립트가 넣은 줄을 지운다 ([switch] 는 값 없이 이름만 쓰는 켜기/끄기 인자)
param(
    [string]$Ip     = '127.0.0.1',
    [string]$Domain = 'sku14.com',
    [switch]$Remove
)

# 오류가 나면 계속 진행하지 않고 바로 멈춘다 (hosts 파일을 반쯤 고친 채로 끝나지 않게)
$ErrorActionPreference = 'Stop'

# hosts 파일: DNS 보다 먼저 참고되는 "이름 → IP" 표. 여기 적으면 인터넷에 등록하지 않은 도메인도 이 PC 에서 열 수 있다.
$hostsPath = Join-Path $env:SystemRoot 'System32\drivers\etc\hosts'
# 이 스크립트가 넣은 줄 끝에 붙이는 표식. 나중에 이 표식이 있는 줄만 골라 지운다.
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
# Where-Object: 조건에 맞는 줄만 남긴다. [regex]::Escape 로 '#' 같은 특수문자를 글자 그대로 찾게 한다.
$kept = $lines | Where-Object { $_ -notmatch [regex]::Escape($marker) }

if ($Remove) {
    Set-Content -LiteralPath $hostsPath -Value $kept -Encoding UTF8
    Write-Host "$Domain 항목을 hosts 에서 제거했습니다." -ForegroundColor Green
}
else {
    # 다른 곳에서 이미 같은 이름을 쓰고 있으면 알려만 주고 중복은 만들지 않는다.
    foreach ($n in $names) {
        # 정규식 뜻: 줄 앞 공백 → 주석(#)이 아닌 IP 같은 낱말 → 공백 → 그 뒤 어딘가에 이 이름이 낱말로 있음
        $conflict = $kept | Where-Object { $_ -match "^\s*[^#]\S*\s+.*\b$([regex]::Escape($n))\b" }
        if ($conflict) {
            Write-Host "주의: hosts 에 $n 항목이 이미 있습니다 -> $conflict" -ForegroundColor Yellow
        }
    }

    # 새 줄 만들기: "IP<탭>이름<탭># sku-contest-board"  (` 다음 t 는 PowerShell 의 탭 문자)
    $added = $names | ForEach-Object { "$Ip`t$_`t$marker" }
    Set-Content -LiteralPath $hostsPath -Value ($kept + $added) -Encoding UTF8

    Write-Host '다음 줄을 hosts 에 넣었습니다.' -ForegroundColor Green
    $added | ForEach-Object { Write-Host "  $_" }
}

# DNS 캐시를 비워 바로 반영되게 한다.
try { ipconfig /flushdns | Out-Null } catch { }

Write-Host ''
Write-Host '확인:' -ForegroundColor Cyan
# 실제로 이름이 어떤 IP 로 풀리는지 .NET DNS 함수로 확인한다
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

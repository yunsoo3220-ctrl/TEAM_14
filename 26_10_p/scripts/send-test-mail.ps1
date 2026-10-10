# smtp.env 설정으로 시험 메일을 한 통 보낸다. 서버를 띄우기 전에 설정이 맞는지 확인할 때 쓴다.
#
#   powershell -ExecutionPolicy Bypass -File scripts\send-test-mail.ps1 -To 학번@skuniv.ac.kr

# param 블록: 스크립트가 받는 인자 정의
#   -To      받는 주소 (Mandatory = 반드시 입력해야 함, 빠뜨리면 PowerShell 이 물어본다)
#   -Project 프로젝트 폴더 (생략하면 이 스크립트의 상위 폴더)
param(
    [Parameter(Mandatory = $true)][string]$To,
    [string]$Project
)

# Mandatory 매개변수가 있으면 기본값 계산 때 $PSScriptRoot 가 비어 있어 여기서 정한다.
$here = Split-Path -Parent $MyInvocation.MyCommand.Path
if (-not $Project) { $Project = Split-Path -Parent $here }

# 점(.) 소싱: 다른 스크립트를 현재 범위에서 실행해 그 안의 함수(Import-SmtpEnv)를 쓸 수 있게 한다
. (Join-Path $here 'smtp-env.ps1')
if (-not (Import-SmtpEnv $Project)) {
    Write-Host 'smtp.env 가 없거나 SKU_SMTP_URL 이 비어 있습니다. smtp.env.example 을 smtp.env 로 복사해 채우세요.' -ForegroundColor Red
    exit 1
}
$env:PATH = 'C:\msys64\ucrt64\bin;' + $env:PATH      # libcurl DLL
Write-Host "보내는 중: $env:SKU_SMTP_URL ($env:SKU_SMTP_USER) -> $To"
# & : 문자열로 된 경로의 프로그램을 실행하는 호출 연산자. server.exe 는 DB 없이 메일만 보내고 끝난다.
& (Join-Path $Project 'server.exe') --test-mail $To
# $LASTEXITCODE: 방금 실행한 프로그램의 종료 코드 (0 = 성공)
if ($LASTEXITCODE -eq 0) { Write-Host '보냈습니다. 받은편지함(스팸함 포함)을 확인하세요.' -ForegroundColor Green }
else { Write-Host '보내지 못했습니다. 위 오류를 확인하세요 (Gmail 은 앱 비밀번호가 필요합니다).' -ForegroundColor Red }
exit $LASTEXITCODE

# smtp.env 설정으로 시험 메일을 한 통 보낸다. 서버를 띄우기 전에 설정이 맞는지 확인할 때 쓴다.
#
#   powershell -ExecutionPolicy Bypass -File scripts\send-test-mail.ps1 -To 학번@skuniv.ac.kr

param(
    [Parameter(Mandatory = $true)][string]$To,
    [string]$Project
)

# Mandatory 매개변수가 있으면 기본값 계산 때 $PSScriptRoot 가 비어 있어 여기서 정한다.
$here = Split-Path -Parent $MyInvocation.MyCommand.Path
if (-not $Project) { $Project = Split-Path -Parent $here }

. (Join-Path $here 'smtp-env.ps1')
if (-not (Import-SmtpEnv $Project)) {
    Write-Host 'smtp.env 가 없거나 SKU_SMTP_URL 이 비어 있습니다. smtp.env.example 을 smtp.env 로 복사해 채우세요.' -ForegroundColor Red
    exit 1
}
$env:PATH = 'C:\msys64\ucrt64\bin;' + $env:PATH      # libcurl DLL
Write-Host "보내는 중: $env:SKU_SMTP_URL ($env:SKU_SMTP_USER) -> $To"
& (Join-Path $Project 'server.exe') --test-mail $To
if ($LASTEXITCODE -eq 0) { Write-Host '보냈습니다. 받은편지함(스팸함 포함)을 확인하세요.' -ForegroundColor Green }
else { Write-Host '보내지 못했습니다. 위 오류를 확인하세요 (Gmail 은 앱 비밀번호가 필요합니다).' -ForegroundColor Red }
exit $LASTEXITCODE

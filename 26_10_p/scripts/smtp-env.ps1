# smtp.env (KEY=VALUE 줄) 를 읽어 현재 프로세스 환경변수로 올린다. 다른 스크립트에서 . 으로 불러 쓴다.
#   . (Join-Path $PSScriptRoot 'smtp-env.ps1'); Import-SmtpEnv $Project

# 반환값: SMTP 설정이 완전하면 $true, 아니면 $false
# smtp.env 예:
#   SKU_SMTP_URL=smtps://smtp.gmail.com:465
#   SKU_SMTP_USER=me@gmail.com
function Import-SmtpEnv([string]$Project) {
    $file = Join-Path $Project 'smtp.env'
    # 이전에 남아 있던 값부터 지운다 (파일에서 지운 설정이 환경변수에 남아 쓰이지 않게)
    foreach ($k in 'SKU_SMTP_URL', 'SKU_SMTP_USER', 'SKU_SMTP_PASS', 'SKU_SMTP_FROM') {
        Remove-Item -Path "env:$k" -ErrorAction SilentlyContinue
    }
    if (-not (Test-Path $file)) { return $false }
    foreach ($line in Get-Content -LiteralPath $file -Encoding UTF8) {
        # 한 줄씩: 앞뒤 공백 제거 → 빈 줄과 # 주석 줄은 건너뜀 → 첫 '=' 기준으로 키와 값을 나눔
        $t = $line.Trim()
        if (-not $t -or $t.StartsWith('#')) { continue }
        $i = $t.IndexOf('=')
        if ($i -lt 1) { continue }
        $k = $t.Substring(0, $i).Trim()
        $v = $t.Substring($i + 1).Trim()
        # 예시 값을 아직 바꾸지 않았으면 설정이 없는 것으로 본다.
        if ($v -match '보내는계정|앱비밀번호|아이디@') { continue }
        # Google 앱 비밀번호는 'abcd efgh ijkl mnop' 처럼 띄어 보여 주므로 공백을 뺀다.
        if ($k -eq 'SKU_SMTP_PASS') { $v = $v -replace '\s', '' }
        # 허용한 키만 환경변수로 올린다 (env: 드라이브에 Set-Item = 현재 프로세스와 그 자식 프로세스의 환경변수)
        if ($k -like 'SKU_SMTP_*' -or $k -eq 'SKU_MAIL_DEV') {
            Set-Item -Path "env:$k" -Value $v
        }
    }
    # 주소·계정·비밀번호가 모두 채워져야 설정된 것으로 본다. 아니면 서버가 반쯤 된 설정으로 뜨지 않게 지운다.
    if ($env:SKU_SMTP_URL -and $env:SKU_SMTP_USER -and $env:SKU_SMTP_PASS) { return $true }
    Remove-Item -Path env:SKU_SMTP_URL -ErrorAction SilentlyContinue
    return $false
}

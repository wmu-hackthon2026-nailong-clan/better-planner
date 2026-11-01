$ErrorActionPreference = 'Stop'
$projectRoot = Split-Path -Parent $PSScriptRoot
Set-Location -LiteralPath $projectRoot
function Invoke-Checked {
    param([string]$Program, [string[]]$Arguments)
    & $Program @Arguments
    if ($LASTEXITCODE -ne 0) { throw "$Program 执行失败，退出码 $LASTEXITCODE" }
}
New-Item -ItemType Directory -Force -Path '.deps' | Out-Null
if (!(Test-Path -LiteralPath '.deps/xuexitong')) {
    Invoke-Checked -Program 'git' -Arguments @('clone', 'https://github.com/DanMo661/xuexitong-cli.git', '.deps/xuexitong')
}
if (!(Test-Path -LiteralPath '.deps/zhs')) {
    Invoke-Checked -Program 'git' -Arguments @('clone', 'https://github.com/pjm314159/zhs.git', '.deps/zhs')
}
Invoke-Checked -Program 'python' -Arguments @('-m', 'venv', '.deps/xxt-env')
Invoke-Checked -Program 'python' -Arguments @('-m', 'venv', '.deps/zhs-env')
$xxtPython = Join-Path $projectRoot '.deps/xxt-env/Scripts/python.exe'
$zhsPython = Join-Path $projectRoot '.deps/zhs-env/Scripts/python.exe'
Invoke-Checked -Program $xxtPython -Arguments @('-m', 'pip', 'install', '-e', '.deps/xuexitong')
Invoke-Checked -Program $zhsPython -Arguments @('-m', 'pip', 'install', '-e', '.deps/zhs')
$cfg = @{
    xxt_command = @($xxtPython, '-m', 'xuexitong_mcp.cli')
    xxt_cwd = (Join-Path $projectRoot '.deps/xuexitong')
    zhs_python = $zhsPython
    sync_interval_minutes = 30
}
if (Test-Path -LiteralPath 'config.local.json') {
    Write-Host '保留已有 config.local.json；建议的配置如下：'
    $cfg | ConvertTo-Json -Depth 4 | Write-Host
} else {
    $cfg | ConvertTo-Json -Depth 4 | Set-Content -Encoding utf8 -LiteralPath 'config.local.json'
}
Write-Host '连接器安装完成。接下来请按 README 配置学习通账号，并运行 .deps/zhs-env/Scripts/zhs.exe login 扫码登录。'

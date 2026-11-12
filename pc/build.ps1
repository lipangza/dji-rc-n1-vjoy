# 编译 VJoy.exe（MinGW-w64 g++）
#
# 用法：
#   .\build.ps1
#
# 需要 advapi32（RegOpenKeyExA），漏掉会链接失败。

$ErrorActionPreference = 'Stop'

$compiler = $null
foreach ($name in @('g++', 'x86_64-w64-mingw32-g++')) {
    $cmd = Get-Command $name -ErrorAction SilentlyContinue
    if ($cmd) { $compiler = $cmd.Source; break }
}

if (-not $compiler) {
    Write-Host '找不到 g++。请先安装 MinGW-w64，并把它的 bin 目录加进 PATH。' -ForegroundColor Red
    Write-Host '也可以手动执行：' -ForegroundColor Yellow
    Write-Host '  g++ -std=c++17 -O2 -static -o VJoy.exe VJoyPC.cpp -lws2_32 -luser32 -ladvapi32'
    exit 1
}

Write-Host "使用编译器：$compiler"

& $compiler -std=c++17 -O2 -static -o VJoy.exe VJoyPC.cpp -lws2_32 -luser32 -ladvapi32

if ($LASTEXITCODE -ne 0) {
    Write-Host "编译失败，退出码 $LASTEXITCODE" -ForegroundColor Red
    exit $LASTEXITCODE
}

$exe = Join-Path $PSScriptRoot 'VJoy.exe'
Write-Host "编译成功：$exe （$((Get-Item -LiteralPath $exe).Length) 字节）" -ForegroundColor Green

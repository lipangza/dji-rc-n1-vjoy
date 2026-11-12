# 构建安卓端 APK
#
# 用法：
#   1. 先配好 JDK 17 与 Android SDK（或设置 JAVA_HOME / ANDROID_HOME 环境变量）
#   2. 按 DJI 官方文档配置 DJI Mobile SDK 依赖（本仓库不含其二进制）
#   3. .\build_apk.ps1
#
# 说明：local.properties 是每台机器自己的，本脚本会在缺失时自动生成。

$ErrorActionPreference = 'Stop'

$proj = $PSScriptRoot
$log  = Join-Path $proj 'build_apk.log'

if (-not $env:JAVA_HOME) {
    Write-Host '没有设置 JAVA_HOME。请先指向 JDK 17 的安装目录。' -ForegroundColor Red
    exit 1
}

if (-not $env:ANDROID_HOME) {
    if ($env:ANDROID_SDK_ROOT) {
        $env:ANDROID_HOME = $env:ANDROID_SDK_ROOT
    } else {
        Write-Host '没有设置 ANDROID_HOME / ANDROID_SDK_ROOT。' -ForegroundColor Red
        exit 1
    }
}

"JAVA_HOME=$env:JAVA_HOME" | Out-File -LiteralPath $log -Encoding utf8
& "$env:JAVA_HOME\bin\java.exe" -version 2>&1 | Out-File -Append -LiteralPath $log -Encoding utf8

Set-Location -LiteralPath $proj

if (-not (Test-Path -LiteralPath (Join-Path $proj 'local.properties'))) {
    "sdk.dir=$($env:ANDROID_HOME -replace '\\','\\')" |
        Out-File -LiteralPath (Join-Path $proj 'local.properties') -Encoding ascii
}

& (Join-Path $proj 'gradlew.bat') --no-daemon assembleDebug 2>&1 |
    Out-File -Append -LiteralPath $log -Encoding utf8

"GRADLE_EXIT=$LASTEXITCODE" | Out-File -Append -LiteralPath $log -Encoding utf8

$apk = Join-Path $proj 'app\build\outputs\apk\debug\app-debug.apk'
if (Test-Path -LiteralPath $apk) {
    $f = Get-Item -LiteralPath $apk
    "APK=$($f.FullName) SIZE=$($f.Length) MTIME=$($f.LastWriteTime)" |
        Out-File -Append -LiteralPath $log -Encoding utf8
    Write-Host "构建完成：$($f.FullName)（$($f.Length) 字节）" -ForegroundColor Green
} else {
    Write-Host "没有找到 APK，请看 $log" -ForegroundColor Red
    exit 1
}

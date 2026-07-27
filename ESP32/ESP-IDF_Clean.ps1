param(
    [string] $ProjectRoot
)

$ErrorActionPreference = 'Stop'
# 双击运行时，出错后保留窗口以便查看错误信息。
trap {
    Write-Host ''
    Write-Host "[ERROR] $($_.Exception.Message)" -ForegroundColor Red
    Write-Host 'Press Enter to exit...'
    Read-Host | Out-Null
    exit 1
}

# 目录自动识别：脚本所在目录名为 scripts 时，取上一级作为项目根目录；否则用脚本所在目录。
$scriptDir = $PSScriptRoot
if ([string]::IsNullOrWhiteSpace($ProjectRoot)) {
    $dirName = Split-Path -Path $scriptDir -Leaf
    if ($dirName -ieq 'scripts') {
        $ProjectRoot = Split-Path -Path $scriptDir -Parent
    }
    else {
        $ProjectRoot = $scriptDir
    }
}
$ProjectRoot = $ProjectRoot.Trim().Trim('"')
$script:Root = (Resolve-Path -LiteralPath $ProjectRoot).Path.TrimEnd('\')
Set-Location -LiteralPath $script:Root
Add-Type -AssemblyName Microsoft.VisualBasic

# 校验 ESP-IDF 项目特征：
#   1) 根目录 CMakeLists.txt 存在且包含 project.cmake 引用
#   2) 存在 main\CMakeLists.txt 或 components 目录（ESP-IDF 标准工程结构）
function Test-EspIdfProject {
    $cmake = Join-Path $script:Root 'CMakeLists.txt'
    if (-not (Test-Path -LiteralPath $cmake -PathType Leaf)) { return $false }
    $content = Get-Content -LiteralPath $cmake -Raw -ErrorAction SilentlyContinue
    if (-not ($content -match 'project\.cmake')) { return $false }
    $hasAppStructure = (Test-Path -LiteralPath (Join-Path $script:Root 'main\CMakeLists.txt')) -or (Test-Path -LiteralPath (Join-Path $script:Root 'components'))
    if (-not $hasAppStructure) { return $false }
    return $true
}
if (-not (Test-EspIdfProject)) {
    Write-Host '未检测到 ESP-IDF 项目特征（缺少 CMakeLists.txt(project.cmake) 或 main\components 目录）…跳过清理' -ForegroundColor Yellow
    for ($i = 5; $i -gt 0; $i--) {
        Write-Host "`r$i 秒后自动退出…" -NoNewline -ForegroundColor Yellow
        Start-Sleep -Seconds 1
    }
    Write-Host ''
    exit 0
}

# 将 ESP-IDF 生成目录和中间文件移入回收站。
function Move-ToRecycleBin {
    param([Parameter(Mandatory = $true)][string] $Path)
    if (-not (Test-Path -LiteralPath $Path)) { Write-Host "[SKIP] $Path"; return }
    $item = Get-Item -LiteralPath $Path -Force
    Write-Host "[RECYCLE] $($item.FullName)"
    if ($item.PSIsContainer) {
        [Microsoft.VisualBasic.FileIO.FileSystem]::DeleteDirectory($item.FullName, [Microsoft.VisualBasic.FileIO.UIOption]::OnlyErrorDialogs, [Microsoft.VisualBasic.FileIO.RecycleOption]::SendToRecycleBin)
        return
    }
    if (($item.Attributes -band [System.IO.FileAttributes]::ReadOnly) -ne 0) { $item.Attributes = $item.Attributes -band (-bnot [System.IO.FileAttributes]::ReadOnly) }
    [Microsoft.VisualBasic.FileIO.FileSystem]::DeleteFile($item.FullName, [Microsoft.VisualBasic.FileIO.UIOption]::OnlyErrorDialogs, [Microsoft.VisualBasic.FileIO.RecycleOption]::SendToRecycleBin)
}

Write-Host "[Clean] Project: $script:Root"
$cleaned = 0
foreach ($path in @('build', '.cache', 'CMakeFiles')) {
    $target = Join-Path $script:Root $path
    if (Test-Path -LiteralPath $target) {
        Move-ToRecycleBin -Path $target
        $cleaned++
    }
}
foreach ($path in @('CMakeCache.txt', 'cmake_install.cmake', 'compile_commands.json', 'project_description.json', 'flasher_args.json', 'flash_args', 'sdkconfig.old')) {
    $target = Join-Path $script:Root $path
    if (Test-Path -LiteralPath $target) {
        Move-ToRecycleBin -Path $target
        $cleaned++
    }
}

if ($cleaned -eq 0) {
    Write-Host '[Clean] No ESP-IDF build artifacts found.'
}
else {
    Write-Host "[Clean] Done. ($cleaned item(s) recycled)"
}

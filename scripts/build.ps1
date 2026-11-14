# SPDX-License-Identifier: GPL-3.0-only
# Build with x86-64 llvm-mingw. The deployed executable remains in bin/.
param([string]$Compiler = 'clang++')
$ErrorActionPreference = 'Stop'
$taskRoot = Split-Path $PSScriptRoot
$taskCompiler = (Get-Command $Compiler -ErrorAction Stop).Source
$taskWindres = Join-Path (Split-Path $taskCompiler) 'llvm-windres.exe'
$taskBuild = Join-Path $taskRoot 'build'
$taskBin = Join-Path $taskRoot 'bin'
New-Item -ItemType Directory -Force -Path $taskBuild, $taskBin | Out-Null
$taskFlags = @(
    '-std=c++17', '-O2', '-municode', '-mwindows', '-static',
    '-Wl,--no-insert-timestamp,--strip-all',
    '-D_WIN32_WINNT=0x0A00', '-DWINVER=0x0A00', '-DNTDDI_VERSION=0x0A00000C',
    '-Wall', '-Wno-missing-braces',
    '-I', (Join-Path $taskRoot 'src'), '-I', (Join-Path $taskRoot 'third_party')
)
$taskLibs = @(
    '-ld3d11', '-ldxgi', '-ldwmapi', '-lwindowscodecs', '-lole32', '-loleaut32',
    '-lshell32', '-lgdiplus', '-lcomdlg32', '-lruntimeobject', '-luser32', '-lgdi32',
    '-ladvapi32', '-luuid', '-luxtheme', '-lmsimg32', '-lversion', '-ltaskschd'
)
$taskSources = (Get-ChildItem -LiteralPath (Join-Path $taskRoot 'src') -Recurse -Filter '*.cpp' -File |
    Sort-Object FullName).FullName
$taskResource = Join-Path $taskBuild 'version.res'
$taskExe = Join-Path $taskBin 'Screenshot.exe'
Push-Location $taskRoot
try {
    & $taskWindres resources\version.rc -O coff -o $taskResource
    if ($LASTEXITCODE -ne 0) { throw 'Resource compilation failed' }
    & $taskCompiler @taskFlags @taskSources $taskResource -o $taskExe @taskLibs
    if ($LASTEXITCODE -ne 0) { throw 'Build failed' }
} finally { Pop-Location }
Get-Item -LiteralPath $taskExe | Select-Object FullName, Length

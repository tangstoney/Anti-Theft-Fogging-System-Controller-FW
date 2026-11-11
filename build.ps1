param(
    [switch]$Clean,
    [string]$SdkRoot = '../../_sdk/Nsingtech.N32WB43x_Library.2.2.0',
    [string]$GccPath = "$env:USERPROFILE/.n32studio/Toolchain/gcc-arm-none-eabi/bin",
    [string]$MakePath = "$env:USERPROFILE/.n32studio/Toolchain/make/make.exe"
)
$ErrorActionPreference = 'Stop'
if (-not (Test-Path -LiteralPath $MakePath -PathType Leaf)) { throw "Missing make: $MakePath" }
if (-not (Test-Path -LiteralPath (Join-Path $GccPath 'arm-none-eabi-gcc.exe'))) { throw "Missing Arm GCC: $GccPath" }
# Make's source-list parsing requires SDK paths without whitespace. This project directory may contain spaces.
if ($SdkRoot -match '\s') { throw 'Use an SDK_ROOT path without spaces (relative paths are supported).' }
if ($GccPath -match '\s') { throw 'Use a GCC toolchain path without spaces for this Makefile.' }
$oldShell = $env:SHELL
$oldMakeShell = $env:MAKESHELL
Push-Location $PSScriptRoot
try {
    if (-not (Test-Path -LiteralPath (Join-Path $SdkRoot 'firmware/CMSIS/device/n32wb43x_flash.ld'))) { throw "Missing SDK: $SdkRoot" }
    $env:SHELL = 'cmd.exe'
    $env:MAKESHELL = 'cmd.exe'
    $makeArgs = @("GCC_PATH=$($GccPath.Replace('\','/'))", "SDK_ROOT=$($SdkRoot.Replace('\','/'))")
    if ($Clean) {
        $buildPath = [IO.Path]::GetFullPath((Join-Path $PSScriptRoot 'build'))
        $expected = [IO.Path]::GetFullPath($PSScriptRoot) + [IO.Path]::DirectorySeparatorChar + 'build'
        if ($buildPath -ne $expected) { throw 'Unexpected build path' }
        if (Test-Path -LiteralPath $buildPath) {
            $buildItem = Get-Item -LiteralPath $buildPath
            if ($buildItem.Attributes -band [IO.FileAttributes]::ReparsePoint) { throw 'Refusing to clean linked build directory' }
            Remove-Item -LiteralPath $buildPath -Recurse -Force
        }
    }
    & $MakePath @makeArgs -j4
    if ($LASTEXITCODE -ne 0) { throw "Build failed: $LASTEXITCODE" }
}
finally {
    Pop-Location
    $env:SHELL = $oldShell
    $env:MAKESHELL = $oldMakeShell
}

# 重新编译
# powershell -NoProfile -ExecutionPolicy Bypass -File .\build.ps1 -Clean
# 增量编译
# powershell -NoProfile -ExecutionPolicy Bypass -File .\build.ps1
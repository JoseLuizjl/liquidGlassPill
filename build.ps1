$ErrorActionPreference = "Stop"

$Compiler = "C:\msys64\mingw64\bin\g++.exe"
$VulkanSdk = "C:\VulkanSDK\1.4.350.0"
$Glslang = Join-Path $VulkanSdk "Bin\glslangValidator.exe"
$BuildDir = Join-Path $PSScriptRoot "build"
$ShaderBuildDir = Join-Path $BuildDir "shaders"
$Output = Join-Path $BuildDir "LiquidGlassPill.exe"

if (-not (Test-Path $BuildDir)) {
    New-Item -ItemType Directory -Path $BuildDir | Out-Null
}
if (-not (Test-Path $ShaderBuildDir)) {
    New-Item -ItemType Directory -Path $ShaderBuildDir | Out-Null
}

function Invoke-Checked {
    param(
        [Parameter(Mandatory = $true)][string]$FilePath,
        [Parameter(Mandatory = $true)][string[]]$Arguments
    )

    & $FilePath @Arguments
    if ($LASTEXITCODE -ne 0) {
        throw "Command failed with exit code $LASTEXITCODE`: $FilePath $Arguments"
    }
}

Invoke-Checked $Glslang @("-V", (Join-Path $PSScriptRoot "shaders\liquid.vert"), "-o", (Join-Path $ShaderBuildDir "liquid.vert.spv"))
Invoke-Checked $Glslang @("-V", (Join-Path $PSScriptRoot "shaders\liquid.frag"), "-o", (Join-Path $ShaderBuildDir "liquid.frag.spv"))

Invoke-Checked $Compiler @(
    "-std=c++17",
    "-O2",
    "-DNDEBUG",
    "-I$VulkanSdk\Include",
    "-Wall",
    "-Wextra",
    "-pedantic",
    "-Wno-missing-field-initializers",
    "-DWIN32_LEAN_AND_MEAN",
    "-DNOMINMAX",
    "-mwindows",
    (Join-Path $PSScriptRoot "src\main.cpp"),
    (Join-Path $VulkanSdk "Lib\vulkan-1.lib"),
    "-lgdi32",
    "-luser32",
    "-o",
    $Output
)

Write-Host "Built $Output"

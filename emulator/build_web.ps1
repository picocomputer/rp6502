# Copyright (c) 2026 Rumbledethumps
# SPDX-License-Identifier: BSD-3-Clause
<#
.SYNOPSIS
    Build the WebAssembly demo into build/web/ on Windows (PowerShell parity
    with tools/build_web.sh).

.DESCRIPTION
    Self-contained: uses the project's OWN Emscripten SDK — the vendor/emsdk
    submodule, pinned to emscripten-version.txt — fetching the toolchain
    INTO that submodule on first run. Nothing is assumed in your user profile and
    no EMSDK variable is needed. After the first run the "wasm" CMake preset works
    directly (CLI or VS Code), since it points at vendor/emsdk's toolchain.

.NOTES
    Requires CMake and Ninja on PATH (Ninja is the generator the preset uses).
#>
$ErrorActionPreference = "Stop"

$emuDir = $PSScriptRoot
$root = Split-Path -Parent $PSScriptRoot
$emsdkDir = Join-Path $root "vendor\emsdk"
$emver = (Get-Content (Join-Path $emuDir "emscripten-version.txt")).Trim()
$emsdkBat = Join-Path $emsdkDir "emsdk.bat"

if (-not (Test-Path $emsdkBat)) {
    Write-Error "The Emscripten SDK submodule is not checked out at '$emsdkDir'. Initialize it: git submodule update --init vendor/emsdk"
    exit 1
}

# One-time: fetch + activate the pinned toolchain into the submodule (~270 MB,
# into vendor\emsdk\ — NOT into your user profile). Skipped once present.
if (-not (Test-Path (Join-Path $emsdkDir "upstream\emscripten\emcc.bat"))) {
    Write-Host "Fetching Emscripten $emver into $emsdkDir (one-time)..."
    & $emsdkBat install $emver
    & $emsdkBat activate $emver
}

foreach ($t in @("cmake", "ninja")) {
    if (-not (Get-Command $t -ErrorAction SilentlyContinue)) {
        Write-Error "'$t' must be on PATH to build the WASM demo."
        exit 1
    }
}

Push-Location $emuDir
try {
    if (Test-Path "build/web") { Remove-Item -Recurse -Force "build/web" }
    cmake --preset wasm
    if ($LASTEXITCODE -ne 0) { throw "configure failed ($LASTEXITCODE)" }
    cmake --build --preset wasm
    if ($LASTEXITCODE -ne 0) { throw "build failed ($LASTEXITCODE)" }

    Write-Host ""
    Write-Host "Deployable web root: build/web/"
    Get-ChildItem -Name "build/web"
    Write-Host ""
    Write-Host "Preview it (must be served over HTTP):"
    Write-Host "    python -m http.server -d build/web 8000"
    Write-Host "    # then visit http://localhost:8000/"
}
finally {
    Pop-Location
}

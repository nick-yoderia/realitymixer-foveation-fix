# Build build\openvr_api.dll with MSVC Build Tools 2022 (no vcvars, no SDK needed:
# the OpenVR 1.26.7 header is in src\openvr-1.26.7).
# src\wrap_*.h are generated: python src\gen_wrap.py src\openvr-1.26.7\openvr.h (run in src\).
$ErrorActionPreference = "Stop"
Set-Location $PSScriptRoot
$vc = "C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\VC\Tools\MSVC"
$msvc = Join-Path $vc (Get-ChildItem $vc | Sort-Object Name | Select-Object -Last 1).Name
$kit = "C:\Program Files (x86)\Windows Kits\10"
$kv = (Get-ChildItem "$kit\Include" | Sort-Object Name | Select-Object -Last 1).Name
$env:INCLUDE = "$msvc\include;$kit\Include\$kv\ucrt;$kit\Include\$kv\um;$kit\Include\$kv\shared"
$env:LIB = "$msvc\lib\x64;$kit\Lib\$kv\ucrt\x64;$kit\Lib\$kv\um\x64"
New-Item -ItemType Directory -Force build | Out-Null
& "$msvc\bin\Hostx64\x64\cl.exe" /nologo /O2 /EHsc /std:c++17 /W3 /D_CRT_SECURE_NO_WARNINGS /LD `
	/Isrc\openvr-1.26.7 /Isrc src\rmfix.cpp /Fobuild\ /Febuild\openvr_api.dll /link d3d11.lib
if ($LASTEXITCODE) { exit $LASTEXITCODE }

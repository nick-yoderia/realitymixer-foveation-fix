# Install (or with -Uninstall remove) the Reality Mixer fix for SteamVR 2.19's
# foveated stream: without it, Reality Mixer's boxes fade out outside the sharp
# gaze area. The fix is an openvr_api.dll in Reality Mixer's folder; the original
# is kept next to it as openvr_api_real.dll. A Reality Mixer update or a Steam
# "verify files" can put the original back; just run this again.
param([switch]$Uninstall)
$ErrorActionPreference = "Stop"

function Find-RealityMixer {
	$steam = (Get-ItemProperty "HKCU:\Software\Valve\Steam" -ErrorAction SilentlyContinue).SteamPath
	if (-not $steam) { $steam = "C:\Program Files (x86)\Steam" }
	$libs = @($steam)
	$vdf = Join-Path $steam "steamapps\libraryfolders.vdf"
	if (Test-Path $vdf) {
		$libs += Select-String -Path $vdf -Pattern '"path"\s+"([^"]+)"' | ForEach-Object { $_.Matches[0].Groups[1].Value -replace '\\\\', '\' }
	}
	foreach ($l in $libs) {
		$d = Join-Path $l "steamapps\common\Reality Mixer"
		if (Test-Path (Join-Path $d "RealityMixer.exe")) { return $d }
	}
	throw "Reality Mixer not found in any Steam library."
}

$dir = Find-RealityMixer
$dll = Join-Path $dir "openvr_api.dll"
$real = Join-Path $dir "openvr_api_real.dll"
$fix = @((Join-Path $PSScriptRoot "bin\openvr_api.dll"), (Join-Path $PSScriptRoot "build\openvr_api.dll")) |
	Where-Object { Test-Path $_ } | Select-Object -First 1
# Ours is small (it forwards to the real one, which is ~800 KB).
function Test-Ours($f) { (Test-Path $f) -and (Get-Item $f).Length -lt 400KB }

$running = Get-Process RealityMixer -ErrorAction SilentlyContinue
if ($running) {
	Write-Host "Closing Reality Mixer..."
	$running | Stop-Process -Force
	$running | Wait-Process -Timeout 15 -ErrorAction SilentlyContinue
}
# Windows can hold the DLL for a moment after the process is gone.
function Wait-Unlocked($f) {
	for ($i = 0; $i -lt 50 -and (Test-Path $f); $i++) {
		try { [IO.File]::Open($f, 'Open', 'ReadWrite', 'None').Close(); return } catch { Start-Sleep -Milliseconds 200 }
	}
}
try {
	Wait-Unlocked $dll

	if ($Uninstall) {
		if (Test-Path $real) {
			if (Test-Path $dll) { Remove-Item -Force $dll }
			Rename-Item $real "openvr_api.dll"
			Write-Host "Reality Mixer fix removed: $dir"
		} else {
			Write-Host "The fix isn't installed."
		}
	} else {
		if (-not $fix) { throw "bin\openvr_api.dll is missing next to this script." }
		# If an update brought back the original DLL, it becomes the new 'real' one.
		if ((Test-Path $dll) -and -not (Test-Ours $dll)) { Move-Item -Force $dll $real }
		if (-not (Test-Path $real)) { throw "Original openvr_api.dll not found in $dir." }
		Copy-Item -Force $fix $dll
		Write-Host "Reality Mixer fix installed: $dir"
	}
} finally {
	if ($running) {
		Write-Host "Starting Reality Mixer again..."
		Start-Process "steam://rungameid/1844610"
	}
}

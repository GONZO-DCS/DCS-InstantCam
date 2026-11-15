param([string]$Mode = "install", [string]$Source = "", [string]$InstDir = "")
# Adds / removes the Instant Cam line in Saved Games\DCS*\Scripts\Export.lua
$line = "local lfs=require('lfs'); dofile(lfs.writedir()..'Scripts/InstantCam/InstantCamExport.lua') -- DCS Instant Cam"
$saved = [Environment]::GetFolderPath("UserProfile") + "\Saved Games"
$dirs = @(Get-ChildItem -Path $saved -Directory -ErrorAction SilentlyContinue | Where-Object { $_.Name -eq "DCS" -or $_.Name -eq "DCS.openbeta" })
if ($dirs.Count -eq 0 -and $Mode -eq "install") {
    New-Item -ItemType Directory -Force -Path "$saved\DCS" | Out-Null
    $dirs = @(Get-Item "$saved\DCS")
}
foreach ($d in $dirs) {
    $scripts = Join-Path $d.FullName "Scripts"
    $export = Join-Path $scripts "Export.lua"
    $icDir = Join-Path $scripts "InstantCam"
    $hooks = Join-Path $scripts "Hooks"
    $hookFile = Join-Path $hooks "InstantCamHook.lua"
    if ($Mode -eq "install") {
        New-Item -ItemType Directory -Force -Path $icDir | Out-Null
        Copy-Item -Force $Source (Join-Path $icDir "InstantCamExport.lua")
        # flat-screen loader: DCS loads the camera DLL through a GUI hook
        New-Item -ItemType Directory -Force -Path $hooks | Out-Null
        $hookLua = (Get-Content -Raw (Join-Path $InstDir "InstantCamHook.lua")).Replace("%INSTDIR%", $InstDir)
        Set-Content -Path $hookFile -Value $hookLua -Encoding ASCII
        if (Test-Path $export) {
            $content = Get-Content -Raw $export
            if ($content -notmatch "InstantCamExport\.lua") {
                Copy-Item -Force $export "$export.instantcam.bak"
                Add-Content -Path $export -Value "`r`n$line"
            }
        } else {
            Set-Content -Path $export -Value $line
        }
    } else {
        if (Test-Path $export) {
            $kept = Get-Content $export | Where-Object { $_ -notmatch "InstantCamExport\.lua" }
            Set-Content -Path $export -Value $kept
        }
        Remove-Item -Recurse -Force $icDir -ErrorAction SilentlyContinue
        Remove-Item -Force $hookFile -ErrorAction SilentlyContinue
    }
}

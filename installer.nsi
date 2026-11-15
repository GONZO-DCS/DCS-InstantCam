; DCS Instant Cam installer
Unicode true
!include "MUI2.nsh"
!include "x64.nsh"

!define APPNAME "DCS Instant Cam"
!define APPVER "1.3.0"
!define REGLAYERS "SOFTWARE\Khronos\OpenXR\1\ApiLayers\Implicit"
!define UNINSTKEY "Software\Microsoft\Windows\CurrentVersion\Uninstall\DCSInstantCam"

Name "${APPNAME} ${APPVER}"
OutFile "dist/DCS_InstantCam_Setup_v1.3.exe"
InstallDir "$PROGRAMFILES64\DCS Instant Cam"
RequestExecutionLevel admin
SetCompressor /SOLID lzma

!define MUI_ABORTWARNING
!define MUI_FINISHPAGE_SHOWREADME "$INSTDIR\README.txt"
!define MUI_FINISHPAGE_SHOWREADME_TEXT "Open the quick guide"
!insertmacro MUI_PAGE_WELCOME
!insertmacro MUI_PAGE_DIRECTORY
!insertmacro MUI_PAGE_INSTFILES
!insertmacro MUI_PAGE_FINISH
!insertmacro MUI_UNPAGE_CONFIRM
!insertmacro MUI_UNPAGE_INSTFILES
!insertmacro MUI_LANGUAGE "English"

Section "Install"
  SetRegView 64
  SetOutPath "$INSTDIR"
  Delete "$INSTDIR\XR_APILAYER_instantcam.dll" ; old name (v1.0/v1.1)
  File "pkg\instantcam.dll"
  File "pkg\XR_APILAYER_instantcam.json"
  File "pkg\shutter.wav"
  File "pkg\empty.wav"
  File "pkg\body.png"
  File "pkg\paper.png"
  File "pkg\ShadowsIntoLight.ttf"
  File "pkg\ShadowsIntoLight-OFL.txt"
  File "pkg\InstantCamHook.lua"
  File "pkg\InstantCamExport.lua"
  File "pkg\export_setup.ps1"
  File "pkg\README.txt"

  ; register the OpenXR implicit API layer (0 = enabled)
  WriteRegDWORD HKLM "${REGLAYERS}" "$INSTDIR\XR_APILAYER_instantcam.json" 0

  ; telemetry hook for photo captions
  DetailPrint "Adding caption telemetry to Export.lua..."
  nsExec::ExecToLog 'powershell.exe -NoProfile -ExecutionPolicy Bypass -File "$INSTDIR\export_setup.ps1" -Mode install -Source "$INSTDIR\InstantCamExport.lua" -InstDir "$INSTDIR"'

  WriteUninstaller "$INSTDIR\Uninstall.exe"
  WriteRegStr HKLM "${UNINSTKEY}" "DisplayName" "${APPNAME}"
  WriteRegStr HKLM "${UNINSTKEY}" "DisplayVersion" "${APPVER}"
  WriteRegStr HKLM "${UNINSTKEY}" "UninstallString" '"$INSTDIR\Uninstall.exe"'
  WriteRegStr HKLM "${UNINSTKEY}" "InstallLocation" "$INSTDIR"
  WriteRegDWORD HKLM "${UNINSTKEY}" "NoModify" 1
  WriteRegDWORD HKLM "${UNINSTKEY}" "NoRepair" 1

  CreateDirectory "$SMPROGRAMS\${APPNAME}"
  CreateShortcut "$SMPROGRAMS\${APPNAME}\Quick guide.lnk" "$INSTDIR\README.txt"
  CreateShortcut "$SMPROGRAMS\${APPNAME}\Photos.lnk" "$PROFILE\Saved Games\InstantCam\Photos"
  CreateShortcut "$SMPROGRAMS\${APPNAME}\Uninstall.lnk" "$INSTDIR\Uninstall.exe"
  CreateDirectory "$PROFILE\Saved Games\InstantCam\Photos"
SectionEnd

Section "Uninstall"
  SetRegView 64
  DeleteRegValue HKLM "${REGLAYERS}" "$INSTDIR\XR_APILAYER_instantcam.json"
  nsExec::ExecToLog 'powershell.exe -NoProfile -ExecutionPolicy Bypass -File "$INSTDIR\export_setup.ps1" -Mode uninstall'
  Delete "$INSTDIR\instantcam.dll"
  Delete "$INSTDIR\XR_APILAYER_instantcam.dll"
  Delete "$INSTDIR\XR_APILAYER_instantcam.json"
  Delete "$INSTDIR\shutter.wav"
  Delete "$INSTDIR\empty.wav"
  Delete "$INSTDIR\body.png"
  Delete "$INSTDIR\paper.png"
  Delete "$INSTDIR\ShadowsIntoLight.ttf"
  Delete "$INSTDIR\ShadowsIntoLight-OFL.txt"
  Delete "$INSTDIR\InstantCamHook.lua"
  Delete "$INSTDIR\InstantCamExport.lua"
  Delete "$INSTDIR\export_setup.ps1"
  Delete "$INSTDIR\README.txt"
  Delete "$INSTDIR\Uninstall.exe"
  RMDir "$INSTDIR"
  RMDir /r "$SMPROGRAMS\${APPNAME}"
  DeleteRegKey HKLM "${UNINSTKEY}"
  ; photos and settings in Saved Games\InstantCam are kept
SectionEnd

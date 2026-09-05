Unicode True
!include "MUI2.nsh"
!include "LogicLib.nsh"
!include "FileFunc.nsh"

!define PRODUCT_NAME "Gambit Record"
!define PRODUCT_VERSION "1.0.0"
Var BackupStamp

Name "${PRODUCT_NAME}"
Caption "${PRODUCT_NAME} Setup"
OutFile "${__FILEDIR__}\..\dist\Gambit-Record-Setup.exe"
InstallDir "C:\Games\GTA San Andreas"
RequestExecutionLevel admin
SetCompressor /SOLID lzma
ShowInstDetails show
ShowUninstDetails show

VIProductVersion "1.0.0.0"
VIAddVersionKey /LANG=1049 "ProductName" "${PRODUCT_NAME}"
VIAddVersionKey /LANG=1049 "FileDescription" "Gambit Record installer for Gambit-RP"
VIAddVersionKey /LANG=1049 "FileVersion" "${PRODUCT_VERSION}"
VIAddVersionKey /LANG=1049 "LegalCopyright" "GPL-3.0-only"

!define MUI_ABORTWARNING
!define MUI_PAGE_CUSTOMFUNCTION_LEAVE VerifyGameDirectory
!insertmacro MUI_PAGE_WELCOME
!insertmacro MUI_PAGE_LICENSE "${__FILEDIR__}\..\LICENSE"
!insertmacro MUI_PAGE_DIRECTORY
!insertmacro MUI_PAGE_INSTFILES
!define MUI_FINISHPAGE_NOAUTOCLOSE
!insertmacro MUI_PAGE_FINISH

!insertmacro MUI_UNPAGE_CONFIRM
!insertmacro MUI_UNPAGE_INSTFILES

!insertmacro MUI_LANGUAGE "Russian"
!insertmacro MUI_LANGUAGE "English"

Function VerifyGameDirectory
  IfFileExists "$INSTDIR\gta_sa.exe" valid invalid
  invalid:
    MessageBox MB_OK|MB_ICONSTOP "gta_sa.exe was not found in the selected directory."
    Abort
  valid:
FunctionEnd

Section "Gambit Record" SEC_MAIN
  SetShellVarContext all
  SetOverwrite on

  ; The worker must not hold its own executable while it is being updated.
  nsExec::ExecToLog '"$SYSDIR\taskkill.exe" /F /IM GambitRecord.exe'

  SetOutPath "$INSTDIR"
  File /oname=grecord.asi "${__FILEDIR__}\payload\grecord.asi"
  File /oname=GambitRecord.exe "${__FILEDIR__}\payload\GambitRecord.exe"

  ; Keep an existing configuration and every recording.
  CreateDirectory "$INSTDIR\grecord"
  SetOutPath "$INSTDIR\grecord"
  SetOverwrite off
  File /oname=config.json "${__FILEDIR__}\payload\config.json"
  SetOverwrite on

  ; One-release migration from the old MoonLoader build.
  IfFileExists "$INSTDIR\moonloader\evidence.lua" 0 migration_done
    ${GetTime} "" "L" $0 $1 $2 $3 $4 $5 $6
    StrCpy $BackupStamp "$2$1$0-$4$5$6"
    Rename "$INSTDIR\moonloader\evidence.lua" "$INSTDIR\moonloader\evidence.lua.disabled-$BackupStamp.bak"
    IfFileExists "$INSTDIR\moonloader\evidence\config.json" 0 migration_done
      CreateDirectory "$INSTDIR\grecord\migration"
      CopyFiles /SILENT "$INSTDIR\moonloader\evidence\config.json" "$INSTDIR\grecord\migration\evidence-config-$BackupStamp.json"
  migration_done:

  WriteUninstaller "$INSTDIR\grecord\Uninstall-Gambit-Record.exe"
SectionEnd

Section "Uninstall"
  nsExec::ExecToLog '"$SYSDIR\taskkill.exe" /F /IM GambitRecord.exe'
  Delete "$INSTDIR\grecord.asi"
  Delete "$INSTDIR\GambitRecord.exe"
  Delete "$INSTDIR\grecord\Uninstall-Gambit-Record.exe"
  ; grecord/config.json, recordings, cache, logs and migration backups are intentionally preserved.
SectionEnd

[Version]
Class=IEXPRESS
SEDVersion=3

[Options]
PackagePurpose=InstallApp
ShowInstallProgramWindow=0
HideExtractAnimation=0
UseLongFileName=1
InsideCompressed=0
CAB_FixedSize=0
CAB_ResvCodeSigning=0
RebootMode=N
InstallPrompt=%InstallPrompt%
DisplayLicense=
FinishMessage=
TargetName=dist\Gambit-Record-Setup.exe
FriendlyName=Gambit Record Setup
AppLaunched=install.cmd
PostInstallCmd=<None>
AdminQuietInstCmd=install.cmd
UserQuietInstCmd=install.cmd
SourceFiles=SourceFiles

[Strings]
InstallPrompt=Install Gambit Record?
FILE0=install.cmd
FILE1=install.ps1
FILE2=payload.zip

[SourceFiles]
SourceFiles0=installer\

[SourceFiles0]
%FILE0%=
%FILE1%=
%FILE2%=

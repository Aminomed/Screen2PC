; Screen2PC Windows x64 Installer (Inno Setup 6)
; Builds Screen2PC-Setup-x64.exe from dist\Screen2PC-Windows-x64

#ifndef AppVersion
  #define AppVersion "1.5.0.0"
#endif
#ifndef DistDir
  #define DistDir "..\dist\Screen2PC-Windows-x64"
#endif
#ifndef OutDir
  #define OutDir "..\dist"
#endif
#ifndef RepoRoot
  #define RepoRoot ".."
#endif

[Setup]
AppId={{8D6B9E1A-5B3C-4E42-9A2A-6F72B3A19C10}
AppName=Screen2PC
AppVersion={#AppVersion}
AppVerName=Screen2PC v{#AppVersion}
AppPublisher=Aminomed
AppPublisherURL=https://github.com/Aminomed/Screen2PC
AppSupportURL=https://github.com/Aminomed/Screen2PC/issues
AppUpdatesURL=https://github.com/Aminomed/Screen2PC/releases
DefaultDirName={autopf}\Screen2PC
DefaultGroupName=Screen2PC
DisableProgramGroupPage=yes
ArchitecturesAllowed=x64compatible
ArchitecturesInstallIn64BitMode=x64compatible
PrivilegesRequired=admin
OutputDir={#OutDir}
OutputBaseFilename=Screen2PC-Setup-x64
SetupIconFile={#RepoRoot}\src\common\assets\Screen2PC.ico
UninstallDisplayIcon={app}\Screen2PC.ico
LicenseFile={#RepoRoot}\LICENSE
Compression=lzma2/ultra64
SolidCompression=yes
WizardStyle=modern
ShowLanguageDialog=auto
CloseApplications=yes
CloseApplicationsFilter=*.exe
AppMutex=Local\Screen2PC_Sender_SingleInstance,Local\Screen2PC_Receiver_SingleInstance
UsePreviousAppDir=yes
UsePreviousTasks=yes

[Languages]
Name: "german"; MessagesFile: "compiler:Languages\German.isl"
Name: "english"; MessagesFile: "compiler:Default.isl"

[CustomMessages]
german.TypeLaptop=Laptop (Sender + Virtueller Display-Treiber)
german.TypePC=PC (Receiver — 100 % treiberfrei + Firewall)
german.TypeCustom=Benutzerdefiniert
german.CompLaptop=Laptop (Sender + Virtueller Display-Treiber)
german.CompPC=PC (Empfaenger — 100 % treiberfrei + Firewall)
german.TaskGroupStartup=Autostart:
german.TaskGroupSetup=Systemeinrichtung:
german.TaskGroupIcons=Verknuepfungen:
german.TaskAutostart=Beim Windows-Start minimiert im System-Tray starten
german.TaskInstallDriver=Virtuellen Display-Treiber installieren (Zertifikat + UMDF v2 IddCx)
german.TaskOpenFirewall=Windows-Firewall-Ports freigeben (TCP 48211-48215, UDP 48210/48215-48217)
german.TaskDesktopIcon=Desktop-Verknuepfung erstellen
german.StatusInstallingDriver=Installiere Screen2PC Virtual Display Treiber (Laptop)...
german.StatusOpeningFirewall=Richte Windows-Firewall-Regeln fuer Screen2PC Receiver ein...
german.StatusConfigAutostart=Konfiguriere Autostart...
german.LaunchAfterInstall=%1 jetzt starten

english.TypeLaptop=Laptop (Sender + Virtual Display Driver)
english.TypePC=PC (Receiver — 100% driver-free + Firewall)
english.TypeCustom=Custom
english.CompLaptop=Laptop (Sender + Virtual Display Driver)
english.CompPC=PC (Receiver — 100% driver-free + Firewall)
english.TaskGroupStartup=Startup:
english.TaskGroupSetup=System Setup:
english.TaskGroupIcons=Shortcuts:
english.TaskAutostart=Start minimized in system tray at Windows logon
english.TaskInstallDriver=Install Virtual Display Driver (certificate + UMDF v2 IddCx)
english.TaskOpenFirewall=Open Windows Firewall ports (TCP 48211-48215, UDP 48210/48215-48217)
english.TaskDesktopIcon=Create desktop shortcut
english.StatusInstallingDriver=Installing Screen2PC Virtual Display Driver (Laptop)...
english.StatusOpeningFirewall=Configuring Windows Firewall rules for Screen2PC Receiver...
english.StatusConfigAutostart=Configuring startup...
english.LaunchAfterInstall=Launch %1 now

[Types]
Name: "laptop"; Description: "{cm:TypeLaptop}"
Name: "pc"; Description: "{cm:TypePC}"
Name: "custom"; Description: "{cm:TypeCustom}"; Flags: iscustom

[Components]
Name: "laptop"; Description: "{cm:CompLaptop}"; Types: laptop; Flags: exclusive disablenouninstallwarning
Name: "pc"; Description: "{cm:CompPC}"; Types: pc; Flags: exclusive disablenouninstallwarning

[Tasks]
Name: "autostart"; Description: "{cm:TaskAutostart}"; GroupDescription: "{cm:TaskGroupStartup}"
Name: "installdriver"; Description: "{cm:TaskInstallDriver}"; Components: laptop; GroupDescription: "{cm:TaskGroupSetup}"
Name: "openfirewall"; Description: "{cm:TaskOpenFirewall}"; Components: pc; GroupDescription: "{cm:TaskGroupSetup}"
Name: "desktopicon"; Description: "{cm:TaskDesktopIcon}"; GroupDescription: "{cm:TaskGroupIcons}"; Flags: unchecked

[InstallDelete]
Type: files; Name: "{app}\Screen2PC.Sender.exe"; Components: pc
Type: files; Name: "{app}\Screen2PC.DriverSetup.exe"; Components: pc
Type: files; Name: "{app}\install_driver.ps1"; Components: pc
Type: files; Name: "{app}\uninstall_driver.ps1"; Components: pc
Type: filesandordirs; Name: "{app}\driver"; Components: pc
Type: files; Name: "{app}\Screen2PC.Receiver.exe"; Components: laptop

[Files]
Source: "{#RepoRoot}\src\common\assets\Screen2PC.ico"; DestDir: "{app}"; Flags: ignoreversion
Source: "{#DistDir}\README.md"; DestDir: "{app}"; Flags: ignoreversion

; Laptop (Sender + UMDF v2 Virtual Display Driver)
Source: "{#DistDir}\Screen2PC.Sender.exe"; DestDir: "{app}"; Components: laptop; Flags: ignoreversion
Source: "{#DistDir}\Screen2PC.DriverSetup.exe"; DestDir: "{app}"; Components: laptop; Flags: ignoreversion
Source: "{#DistDir}\install_driver.ps1"; DestDir: "{app}"; Components: laptop; Flags: ignoreversion
Source: "{#DistDir}\uninstall_driver.ps1"; DestDir: "{app}"; Components: laptop; Flags: ignoreversion
Source: "{#DistDir}\driver\*"; DestDir: "{app}\driver"; Components: laptop; Flags: ignoreversion recursesubdirs createallsubdirs

; PC (Receiver — strictly driver-free)
Source: "{#DistDir}\Screen2PC.Receiver.exe"; DestDir: "{app}"; Components: pc; Flags: ignoreversion

[Icons]
Name: "{group}\Screen2PC Sender"; Filename: "{app}\Screen2PC.Sender.exe"; IconFilename: "{app}\Screen2PC.ico"; Components: laptop
Name: "{group}\Screen2PC Receiver"; Filename: "{app}\Screen2PC.Receiver.exe"; IconFilename: "{app}\Screen2PC.ico"; Components: pc
Name: "{group}\{cm:UninstallProgram,Screen2PC}"; Filename: "{uninstallexe}"
Name: "{autodesktop}\Screen2PC Sender"; Filename: "{app}\Screen2PC.Sender.exe"; IconFilename: "{app}\Screen2PC.ico"; Components: laptop; Tasks: desktopicon
Name: "{autodesktop}\Screen2PC Receiver"; Filename: "{app}\Screen2PC.Receiver.exe"; IconFilename: "{app}\Screen2PC.ico"; Components: pc; Tasks: desktopicon

[Run]
; Laptop: install UMDF virtual display driver & self-signed catalog + allow remote connect from trusted PCs (UDP 48218)
Filename: "powershell.exe"; Parameters: "-NoProfile -ExecutionPolicy Bypass -File ""{app}\install_driver.ps1"""; StatusMsg: "{cm:StatusInstallingDriver}"; Components: laptop; Tasks: installdriver; Flags: waituntilterminated
Filename: "{sys}\netsh.exe"; Parameters: "advfirewall firewall delete rule name=""Screen2PC Sender Remote"""; Components: laptop; Flags: runhidden waituntilterminated
Filename: "{sys}\netsh.exe"; Parameters: "advfirewall firewall add rule name=""Screen2PC Sender Remote"" dir=in action=allow protocol=UDP localport=48218"; Components: laptop; Flags: runhidden waituntilterminated

; PC: configure Windows Firewall rules
Filename: "{sys}\netsh.exe"; Parameters: "advfirewall firewall delete rule name=""Screen2PC"""; StatusMsg: "{cm:StatusOpeningFirewall}"; Components: pc; Tasks: openfirewall; Flags: runhidden waituntilterminated
Filename: "{sys}\netsh.exe"; Parameters: "advfirewall firewall delete rule name=""Screen2PC Discovery"""; StatusMsg: "{cm:StatusOpeningFirewall}"; Components: pc; Tasks: openfirewall; Flags: runhidden waituntilterminated
Filename: "{sys}\netsh.exe"; Parameters: "advfirewall firewall add rule name=""Screen2PC"" dir=in action=allow protocol=TCP localport=48211-48215"; StatusMsg: "{cm:StatusOpeningFirewall}"; Components: pc; Tasks: openfirewall; Flags: runhidden waituntilterminated
Filename: "{sys}\netsh.exe"; Parameters: "advfirewall firewall add rule name=""Screen2PC Discovery"" dir=in action=allow protocol=UDP localport=48210,48215,48216,48217"; StatusMsg: "{cm:StatusOpeningFirewall}"; Components: pc; Tasks: openfirewall; Flags: runhidden waituntilterminated

; Configure or clear autostart via the executables themselves
Filename: "{app}\Screen2PC.Sender.exe"; Parameters: "--install-autostart"; StatusMsg: "{cm:StatusConfigAutostart}"; Components: laptop; Tasks: autostart; Flags: runhidden waituntilterminated
Filename: "{app}\Screen2PC.Sender.exe"; Parameters: "--uninstall-autostart"; StatusMsg: "{cm:StatusConfigAutostart}"; Components: laptop; Tasks: not autostart; Flags: runhidden waituntilterminated
Filename: "{app}\Screen2PC.Receiver.exe"; Parameters: "--install-autostart"; StatusMsg: "{cm:StatusConfigAutostart}"; Components: pc; Tasks: autostart; Flags: runasoriginaluser runhidden waituntilterminated
Filename: "{app}\Screen2PC.Receiver.exe"; Parameters: "--uninstall-autostart"; StatusMsg: "{cm:StatusConfigAutostart}"; Components: pc; Tasks: not autostart; Flags: runasoriginaluser runhidden waituntilterminated

; Optional post-install launch
Filename: "{app}\Screen2PC.Sender.exe"; Description: "{cm:LaunchAfterInstall,Screen2PC Sender}"; Components: laptop; Flags: nowait postinstall skipifsilent
Filename: "{app}\Screen2PC.Receiver.exe"; Description: "{cm:LaunchAfterInstall,Screen2PC Receiver}"; Components: pc; Flags: runasoriginaluser nowait postinstall skipifsilent

[UninstallRun]
Filename: "{app}\Screen2PC.Sender.exe"; Parameters: "--uninstall-autostart"; Components: laptop; Flags: runhidden waituntilterminated skipifdoesntexist; RunOnceId: "UninstAutostartSender"
Filename: "{app}\Screen2PC.Receiver.exe"; Parameters: "--uninstall-autostart"; Components: pc; Flags: runhidden waituntilterminated skipifdoesntexist; RunOnceId: "UninstAutostartReceiver"
Filename: "powershell.exe"; Parameters: "-NoProfile -ExecutionPolicy Bypass -File ""{app}\uninstall_driver.ps1"""; Components: laptop; Flags: runhidden waituntilterminated skipifdoesntexist; RunOnceId: "UninstDriverLaptop"
Filename: "{sys}\netsh.exe"; Parameters: "advfirewall firewall delete rule name=""Screen2PC Sender Remote"""; Components: laptop; Flags: runhidden waituntilterminated; RunOnceId: "UninstFwSenderRemote"
Filename: "{sys}\netsh.exe"; Parameters: "advfirewall firewall delete rule name=""Screen2PC"""; Components: pc; Flags: runhidden waituntilterminated; RunOnceId: "UninstFwTcp"
Filename: "{sys}\netsh.exe"; Parameters: "advfirewall firewall delete rule name=""Screen2PC Discovery"""; Components: pc; Flags: runhidden waituntilterminated; RunOnceId: "UninstFwUdp"

[Code]
procedure ForceCloseRunningScreen2PC;
var
  ResultCode: Integer;
begin
  Exec(ExpandConstant('{sys}\taskkill.exe'), '/F /IM Screen2PC.Sender.exe /T', '', SW_HIDE, ewWaitUntilTerminated, ResultCode);
  Exec(ExpandConstant('{sys}\taskkill.exe'), '/F /IM Screen2PC.Receiver.exe /T', '', SW_HIDE, ewWaitUntilTerminated, ResultCode);
end;

procedure CurStepChanged(CurStep: TSetupStep);
begin
  if CurStep = ssInstall then
  begin
    ForceCloseRunningScreen2PC;
  end;
end;

function InitializeUninstall(): Boolean;
begin
  ForceCloseRunningScreen2PC;
  Result := True;
end;

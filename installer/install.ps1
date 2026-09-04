param([string]$TargetPath = '', [switch]$NonInteractive, [switch]$SkipElevation)
$ErrorActionPreference = 'Stop'

function Show-Result([string]$Message, [bool]$Error = $false) {
    if ($NonInteractive) { if ($Error) { Write-Error $Message } else { Write-Output $Message }; return }
    Add-Type -AssemblyName System.Windows.Forms
    $icon = if ($Error) { [Windows.Forms.MessageBoxIcon]::Error } else { [Windows.Forms.MessageBoxIcon]::Information }
    [void][Windows.Forms.MessageBox]::Show($Message, 'Gambit Record Setup', 'OK', $icon)
}

try {
    if (-not $SkipElevation) {
        $principal = [Security.Principal.WindowsPrincipal]::new([Security.Principal.WindowsIdentity]::GetCurrent())
        if (-not $principal.IsInRole([Security.Principal.WindowsBuiltInRole]::Administrator)) {
            $arguments = @('-NoProfile', '-ExecutionPolicy', 'Bypass', '-File', ('"{0}"' -f $PSCommandPath))
            $process = Start-Process powershell.exe -Verb RunAs -ArgumentList $arguments -Wait -PassThru -WindowStyle Hidden
            exit $process.ExitCode
        }
    }
    if ([string]::IsNullOrWhiteSpace($TargetPath)) {
        Add-Type -AssemblyName System.Windows.Forms
        $dialog = [Windows.Forms.FolderBrowserDialog]::new()
        $dialog.Description = 'Выберите папку GTA San Andreas с gta_sa.exe'
        if (Test-Path -LiteralPath 'C:\Games\GTA San Andreas\gta_sa.exe') { $dialog.SelectedPath = 'C:\Games\GTA San Andreas' }
        if ($dialog.ShowDialog() -ne [Windows.Forms.DialogResult]::OK) { exit 1 }
        $TargetPath = $dialog.SelectedPath
    }
    $TargetPath = [IO.Path]::GetFullPath($TargetPath)
    if (-not (Test-Path -LiteralPath (Join-Path $TargetPath 'gta_sa.exe') -PathType Leaf)) { Show-Result 'gta_sa.exe не найден.' $true; exit 2 }
    $payload = Join-Path $PSScriptRoot 'payload.zip'
    if (-not (Test-Path -LiteralPath $payload -PathType Leaf)) { throw 'payload.zip не найден.' }
    $tempRoot = Join-Path ([IO.Path]::GetTempPath()) ('GambitRecord-' + [Guid]::NewGuid().ToString('N'))
    [IO.Directory]::CreateDirectory($tempRoot) | Out-Null
    try {
        Expand-Archive -LiteralPath $payload -DestinationPath $tempRoot -Force
        $worker = Join-Path $TargetPath 'GambitRecord.exe'
        Get-CimInstance Win32_Process -Filter "Name='GambitRecord.exe'" | Where-Object { $_.ExecutablePath -eq $worker } | ForEach-Object { Stop-Process -Id $_.ProcessId -Force }
        [IO.Directory]::CreateDirectory((Join-Path $TargetPath 'grecord')) | Out-Null
        Copy-Item -LiteralPath (Join-Path $tempRoot 'grecord.asi') -Destination (Join-Path $TargetPath 'grecord.asi') -Force
        Copy-Item -LiteralPath (Join-Path $tempRoot 'GambitRecord.exe') -Destination $worker -Force
        $config = Join-Path $TargetPath 'grecord\config.json'
        if (-not (Test-Path -LiteralPath $config)) { Copy-Item -LiteralPath (Join-Path $tempRoot 'config.json') -Destination $config }

        # One-release migration: disable only the old script, preserve settings and every recording.
        $legacyScript = Join-Path $TargetPath 'moonloader\evidence.lua'
        if (Test-Path -LiteralPath $legacyScript -PathType Leaf) {
            $stamp = Get-Date -Format 'yyyyMMdd-HHmmss'
            Move-Item -LiteralPath $legacyScript -Destination ($legacyScript + '.disabled-' + $stamp + '.bak')
            $legacyConfig = Join-Path $TargetPath 'moonloader\evidence\config.json'
            if (Test-Path -LiteralPath $legacyConfig -PathType Leaf) {
                $migration = Join-Path $TargetPath 'grecord\migration'
                [IO.Directory]::CreateDirectory($migration) | Out-Null
                Copy-Item -LiteralPath $legacyConfig -Destination (Join-Path $migration ('evidence-config-' + $stamp + '.json'))
            }
        }
    } finally {
        $resolved = [IO.Path]::GetFullPath($tempRoot)
        $tempBase = [IO.Path]::GetFullPath([IO.Path]::GetTempPath())
        if ($resolved.StartsWith($tempBase, [StringComparison]::OrdinalIgnoreCase) -and (Split-Path -Leaf $resolved).StartsWith('GambitRecord-')) {
            Remove-Item -LiteralPath $resolved -Recurse -Force -ErrorAction SilentlyContinue
        }
    }
    Show-Result 'Gambit Record установлен. Запустите GTA и используйте /grecord.'
    exit 0
} catch { Show-Result ('Ошибка установки: ' + $_.Exception.Message) $true; exit 10 }

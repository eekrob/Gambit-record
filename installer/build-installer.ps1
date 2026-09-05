param([Parameter(Mandatory)][string]$AsiPath, [Parameter(Mandatory)][string]$WorkerPath)
$ErrorActionPreference = 'Stop'
$repo = [IO.Path]::GetFullPath((Join-Path $PSScriptRoot '..'))
$payloadRoot = Join-Path $PSScriptRoot 'payload'
if (-not ([IO.Path]::GetFullPath($payloadRoot).StartsWith($repo, [StringComparison]::OrdinalIgnoreCase))) { throw 'Invalid payload path' }
if (Test-Path -LiteralPath $payloadRoot) { Remove-Item -LiteralPath $payloadRoot -Recurse -Force }
[IO.Directory]::CreateDirectory($payloadRoot) | Out-Null
Copy-Item -LiteralPath $AsiPath -Destination (Join-Path $payloadRoot 'grecord.asi')
Copy-Item -LiteralPath $WorkerPath -Destination (Join-Path $payloadRoot 'GambitRecord.exe')
Copy-Item -LiteralPath (Join-Path $repo 'config.example.json') -Destination (Join-Path $payloadRoot 'config.json')
[IO.Directory]::CreateDirectory((Join-Path $repo 'dist')) | Out-Null
$candidates = @(
    (Join-Path ${env:ProgramFiles(x86)} 'NSIS\makensis.exe'),
    (Join-Path $env:ProgramFiles 'NSIS\makensis.exe')
) | Where-Object { $_ -and (Test-Path -LiteralPath $_ -PathType Leaf) }
$makensis = Get-Command makensis.exe -ErrorAction SilentlyContinue | Select-Object -ExpandProperty Source -First 1
if (-not $makensis) { $makensis = $candidates | Select-Object -First 1 }
if (-not $makensis) { throw 'NSIS 3 is required (makensis.exe was not found).' }

$output = Join-Path $repo 'dist\Gambit-Record-Setup.exe'
if (Test-Path -LiteralPath $output) { Remove-Item -LiteralPath $output -Force }
& $makensis /V2 (Join-Path $PSScriptRoot 'gambit-record.nsi')
if ($LASTEXITCODE) { throw "makensis failed: $LASTEXITCODE" }
if (-not (Test-Path -LiteralPath $output -PathType Leaf)) { throw 'NSIS did not create the installer.' }

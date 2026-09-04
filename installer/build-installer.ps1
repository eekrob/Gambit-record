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
$archive = Join-Path $PSScriptRoot 'payload.zip'
if (Test-Path -LiteralPath $archive) { Remove-Item -LiteralPath $archive -Force }
[IO.Directory]::CreateDirectory((Join-Path $repo 'dist')) | Out-Null
$payloadFiles = Get-ChildItem -LiteralPath $payloadRoot -File | Select-Object -ExpandProperty FullName
Compress-Archive -LiteralPath $payloadFiles -DestinationPath $archive
Push-Location $repo
try { & iexpress.exe /N /Q (Join-Path $PSScriptRoot 'gambit-record-installer.sed'); if ($LASTEXITCODE) { throw "IExpress failed: $LASTEXITCODE" } }
finally { Pop-Location }

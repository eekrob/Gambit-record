$ErrorActionPreference = 'Stop'
$patterns = @(
    ('ya' + '29\.[A-Za-z0-9_-]{20,}'),
    ('1/' + '/[A-Za-z0-9_-]{20,}'),
    ('AI' + 'za[0-9A-Za-z_-]{20,}'),
    ('-----BEGIN ' + '(RSA |EC |OPENSSH )?PRIVATE KEY-----'),
    ('client_' + 'secret\s*[=:]\s*["''][^"'']{12,}')
)
$files = git ls-files --cached --others --exclude-standard
$hits = @()
foreach ($file in $files) {
    if (-not (Test-Path -LiteralPath $file -PathType Leaf)) { continue }
    try { $text = [IO.File]::ReadAllText([IO.Path]::GetFullPath($file)) } catch { continue }
    foreach ($pattern in $patterns) {
        if ([regex]::IsMatch($text, $pattern)) { $hits += $file; break }
    }
}
if ($hits.Count) {
    $hits | Sort-Object -Unique | ForEach-Object { Write-Error "Possible secret in $_" }
    exit 1
}
Write-Output "Secret scan passed ($($files.Count) files)."

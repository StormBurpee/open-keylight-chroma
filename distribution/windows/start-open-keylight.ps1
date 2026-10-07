# Portable, per-user bootstrap. The TUI alone owns discovery and installation.
[CmdletBinding()]
param([Parameter(ValueFromRemainingArguments = $true)][string[]]$InstallerArguments)

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'

function Assert-Archive([string]$Path, $Spec) {
    if (-not (Test-Path -LiteralPath $Path -PathType Leaf)) { return $false }
    $file = Get-Item -LiteralPath $Path
    if ($file.Length -ne $Spec.bytes) { return $false }
    # Get-FileHash is not reliably available when PS 7 launches this PS 5.1
    # script through CMD with an inherited Core module path. Hash via .NET
    # without rewriting the user's module search path.
    $algorithm = [Security.Cryptography.SHA256]::Create()
    $stream = $null
    try {
        $stream = [IO.File]::OpenRead($Path)
        $hash = [BitConverter]::ToString($algorithm.ComputeHash($stream)).Replace('-', '').ToLowerInvariant()
        return $hash -ceq $Spec.sha256
    }
    finally {
        if ($null -ne $stream) { $stream.Dispose() }
        $algorithm.Dispose()
    }
}

function Get-VerifiedArchive([string]$Cache, $Spec) {
    $uri = [Uri]$Spec.url
    if ($uri.Scheme -cne 'https' -or $uri.Host -notin @('nodejs.org', 'www.python.org') -or
        $Spec.sha256 -cnotmatch '^[a-f0-9]{64}$' -or $Spec.bytes -lt 1 -or $Spec.bytes -gt 67108864) {
        throw 'The runtime manifest is invalid.'
    }
    $archive = Join-Path $Cache ($Spec.sha256 + '.zip')
    if (Test-Path -LiteralPath $archive) {
        if (-not (Assert-Archive $archive $Spec)) { throw "Cached runtime is damaged. Remove this file and launch again: $archive" }
        return $archive
    }
    $temporary = Join-Path $Cache ([Guid]::NewGuid().ToString('N') + '.download')
    $handler = [System.Net.Http.HttpClientHandler]::new()
    $handler.AllowAutoRedirect = $false
    $client = [System.Net.Http.HttpClient]::new($handler)
    $cancel = [System.Threading.CancellationTokenSource]::new(120000)
    $response = $null; $inputStream = $null; $outputStream = $null
    try {
        $response = $client.GetAsync($uri, [System.Net.Http.HttpCompletionOption]::ResponseHeadersRead, $cancel.Token).GetAwaiter().GetResult()
        if ([int]$response.StatusCode -ne 200 -or $response.Content.Headers.ContentLength -ne $Spec.bytes) {
            throw 'The runtime publisher returned an unexpected response.'
        }
        $inputStream = $response.Content.ReadAsStreamAsync().GetAwaiter().GetResult()
        $outputStream = [IO.File]::Open($temporary, [IO.FileMode]::CreateNew, [IO.FileAccess]::Write, [IO.FileShare]::None)
        $buffer = [byte[]]::new(65536); $total = 0L
        while (($count = $inputStream.ReadAsync($buffer, 0, $buffer.Length, $cancel.Token).GetAwaiter().GetResult()) -gt 0) {
            $total += $count
            if ($total -gt $Spec.bytes) { throw 'The runtime download exceeded its expected size.' }
            $outputStream.Write($buffer, 0, $count)
        }
        $outputStream.Flush($true); $outputStream.Dispose(); $outputStream = $null
        if (-not (Assert-Archive $temporary $Spec)) { throw 'Runtime verification failed. Nothing was executed.' }
        try { [IO.File]::Move($temporary, $archive) }
        catch {
            if (-not (Assert-Archive $archive $Spec)) { throw }
        }
        return $archive
    }
    finally {
        if ($null -ne $outputStream) { $outputStream.Dispose() }
        if ($null -ne $inputStream) { $inputStream.Dispose() }
        if ($null -ne $response) { $response.Dispose() }
        $cancel.Dispose(); $client.Dispose(); $handler.Dispose()
        if (Test-Path -LiteralPath $temporary) { Remove-Item -LiteralPath $temporary }
    }
}

function Expand-Runtime([string]$Archive, [string]$Destination, $Spec, [bool]$NodeOnly) {
    [IO.Directory]::CreateDirectory($Destination) | Out-Null
    $zip = [IO.Compression.ZipFile]::OpenRead($Archive)
    $seen = [Collections.Generic.HashSet[string]]::new([StringComparer]::OrdinalIgnoreCase)
    $total = 0L
    $buffer = [byte[]]::new(65536)
    try {
        foreach ($entry in $zip.Entries) {
            if (-not $entry.FullName.StartsWith($Spec.prefix, [StringComparison]::Ordinal)) { throw 'Unexpected runtime archive layout.' }
            $name = $entry.FullName.Substring($Spec.prefix.Length)
            if ($NodeOnly -and $name -notin @('node.exe', 'LICENSE')) { continue }
            if ($name -notmatch '^[A-Za-z0-9_.-]+$' -or $name -in @('.', '..') -or -not $seen.Add($name)) {
                throw 'Unexpected or duplicate runtime archive entry.'
            }
            if ($entry.Length -le 0 -or $entry.Length -gt (167772160 - $total)) { throw 'Runtime extraction exceeds its bounds.' }
            # ZIP metadata alone does not bound DeflateStream/ExtractToFile.
            # Validate the actual output before every write, and exact EOF.
            $source = $null; $target = $null; $written = 0L
            try {
                $source = $entry.Open()
                $target = [IO.File]::Open((Join-Path $Destination $name), [IO.FileMode]::CreateNew,
                    [IO.FileAccess]::Write, [IO.FileShare]::None)
                while (($count = $source.Read($buffer, 0, $buffer.Length)) -gt 0) {
                    if (($written + $count) -gt $entry.Length -or ($total + $count) -gt 167772160) {
                        throw 'Runtime extraction exceeds its declared length or size bounds.'
                    }
                    $target.Write($buffer, 0, $count)
                    $written += $count; $total += $count
                }
                if ($written -ne $entry.Length) { throw 'Runtime archive entry length differs.' }
            }
            finally {
                if ($null -ne $target) { $target.Dispose() }
                if ($null -ne $source) { $source.Dispose() }
            }
        }
        $expected = if ($NodeOnly) { 'node.exe' } else { 'python.exe' }
        if (-not $seen.Contains($expected)) { throw 'Runtime executable is missing.' }
    }
    finally { $zip.Dispose() }
}

function Start-OpenKeylight([string]$Root, [string[]]$Arguments) {
    $architecture = if ($env:PROCESSOR_ARCHITEW6432) { $env:PROCESSOR_ARCHITEW6432 } else { $env:PROCESSOR_ARCHITECTURE }
    if ($architecture -ne 'AMD64') { throw 'This download is for Windows x64. See Getting Started for other platforms.' }
    foreach ($file in @('installer/cli.js', 'tools/stock_migration.py', 'firmware/bundle.json')) {
        if (-not (Test-Path -LiteralPath (Join-Path $Root $file) -PathType Leaf)) { throw 'Extract the entire release ZIP before opening the launcher.' }
    }
    Add-Type -AssemblyName System.Net.Http
    Add-Type -AssemblyName System.IO.Compression.FileSystem
    [Net.ServicePointManager]::SecurityProtocol = [Net.SecurityProtocolType]::Tls12
    $manifest = Get-Content -LiteralPath (Join-Path $Root 'runtimes.json') -Raw | ConvertFrom-Json
    if ($manifest.format -ne 1 -or $manifest.platform -cne 'win-x64') { throw 'Unsupported runtime manifest.' }
    $cache = Join-Path $env:LOCALAPPDATA 'OpenKeylight/runtime-archives'
    [IO.Directory]::CreateDirectory($cache) | Out-Null
    $session = Join-Path $cache ('session-' + [Guid]::NewGuid().ToString('N'))
    [IO.Directory]::CreateDirectory($session) | Out-Null
    try {
        Write-Host ''
        Write-Host '  OPEN KEYLIGHT' -ForegroundColor Cyan
        Write-Host '  Preparing your local installer. First launch downloads about 47 MB.'
        $nodeArchive = Get-VerifiedArchive $cache $manifest.node
        $pythonArchive = Get-VerifiedArchive $cache $manifest.python
        Expand-Runtime $nodeArchive (Join-Path $session 'node') $manifest.node $true
        Expand-Runtime $pythonArchive (Join-Path $session 'python') $manifest.python $false
        $node = Join-Path $session 'node/node.exe'
        $python = Join-Path $session 'python/python.exe'
        & $node (Join-Path $Root 'installer/cli.js') --root $Root --bundle (Join-Path $Root 'firmware/bundle.json') --python $python @Arguments
        if ($LASTEXITCODE -ne 0) { throw "The installer stopped (exit $LASTEXITCODE). Its recovery details remain in the terminal above." }
    }
    finally {
        $resolvedSession = [IO.Path]::GetFullPath($session)
        $resolvedCache = [IO.Path]::GetFullPath($cache).TrimEnd('\') + '\'
        if (-not $resolvedSession.StartsWith($resolvedCache, [StringComparison]::OrdinalIgnoreCase) -or
            [IO.Path]::GetFileName($resolvedSession) -notmatch '^session-[a-f0-9]{32}$') { throw 'Unexpected runtime cleanup path.' }
        if (Test-Path -LiteralPath $resolvedSession) { Remove-Item -LiteralPath $resolvedSession -Recurse -Force }
    }
}

if ($MyInvocation.InvocationName -ne '.') {
    try { Start-OpenKeylight $PSScriptRoot $InstallerArguments }
    catch { Write-Host "`n  $($_.Exception.Message)`n" -ForegroundColor Red; exit 1 }
}

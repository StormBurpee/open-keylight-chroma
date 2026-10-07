# Actual launcher functions, synthetic archives, local Node only. No downloads.
[CmdletBinding()]
param(
    [string]$Launcher = '',
    [string]$NodeExe = ''
)
Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'
if (-not $Launcher) { $Launcher = Join-Path $PSScriptRoot '../../distribution/windows/start-open-keylight.ps1' }
Add-Type -AssemblyName System.IO.Compression.FileSystem
Add-Type -AssemblyName System.IO.Compression
Add-Type -AssemblyName System.Net.Http
. $Launcher

$script:Checks = 0
$script:Cases = 0
$script:Base = Join-Path ([IO.Path]::GetTempPath()) ('okl-launcher-test-' + [Guid]::NewGuid().ToString('N'))
[IO.Directory]::CreateDirectory($script:Base) | Out-Null
$oldLocal = $env:LOCALAPPDATA
$oldArchitecture = $env:PROCESSOR_ARCHITECTURE
$oldArchitecture32 = $env:PROCESSOR_ARCHITEW6432
$oldExit = $env:OKL_TEST_EXIT
$oldCapture = $env:OKL_TEST_CAPTURE
$script:ReadArchive = (Get-Command Get-VerifiedArchive).ScriptBlock
$script:ExpandArchive = (Get-Command Expand-Runtime).ScriptBlock

function Check([bool]$Condition, [string]$Message) {
    $script:Checks++
    if (-not $Condition) { throw "Assertion failed: $Message" }
}
function Throws([scriptblock]$Action, [string]$Pattern = '.') {
    $caught = $false
    try { & $Action | Out-Null }
    catch {
        $caught = $true
        Check ($_.Exception.Message -match $Pattern) "Expected '$Pattern', got '$($_.Exception.Message)'"
    }
    Check $caught 'Expected failure, no silent success'
}
function New-Case([string]$Name) {
    $script:Cases++
    $path = Join-Path $script:Base ($script:Cases.ToString() + '-' + $Name)
    [IO.Directory]::CreateDirectory($path) | Out-Null
    return $path
}
function Make-Zip([string]$Path, [object[]]$Entries) {
    $file = [IO.File]::Open($Path, [IO.FileMode]::CreateNew)
    $zip = [IO.Compression.ZipArchive]::new($file, [IO.Compression.ZipArchiveMode]::Create, $false)
    try {
        foreach ($item in $Entries) {
            $entry = $zip.CreateEntry($item[0])
            $stream = $entry.Open()
            try {
                $bytes = [Text.Encoding]::UTF8.GetBytes($item[1])
                $stream.Write($bytes, 0, $bytes.Length)
            }
            finally { $stream.Dispose() }
        }
    }
    finally { $zip.Dispose(); $file.Dispose() }
}
function Spec([string]$Path, [string]$Prefix = '') {
    return [pscustomobject]@{url = 'https://www.python.org/fixture.zip'; bytes = (Get-Item -LiteralPath $Path).Length;
        sha256 = (Get-FileHash -LiteralPath $Path -Algorithm SHA256).Hash.ToLowerInvariant(); prefix = $Prefix}
}
function No-Sessions {
    $cache = Join-Path $env:LOCALAPPDATA 'OpenKeylight/runtime-archives'
    $sessions = @(Get-ChildItem -LiteralPath $cache -Directory -Filter 'session-*' -ErrorAction SilentlyContinue)
    Check ($sessions.Count -eq 0) 'Session runtime directories must be cleaned on every terminal outcome'
}

try {
    $dir = New-Case 'verified-cache'
    $zip = Join-Path $dir 'input.zip'
    Make-Zip $zip @(@('python.exe', 'python'), @('python313.zip', 'stdlib'))
    $spec = Spec $zip
    Check (Assert-Archive $zip $spec) 'Exact archive accepted'
    Check (-not (Assert-Archive (Join-Path $dir 'absent.zip') $spec)) 'Missing archive rejected'
    $cached = Join-Path $dir ($spec.sha256 + '.zip')
    [IO.File]::Copy($zip, $cached)
    Check ((Get-VerifiedArchive $dir $spec) -ceq $cached) 'Pinned existing cache used without HTTP'
    $changed = [IO.File]::ReadAllBytes($cached)
    $changed[0] = $changed[0] -bxor 1
    [IO.File]::WriteAllBytes($cached, $changed)
    Check (-not (Assert-Archive $cached $spec)) 'Same-length cache corruption rejected'
    Throws { Get-VerifiedArchive $dir $spec } 'Cached runtime is damaged'
    [IO.File]::WriteAllBytes($cached, [byte[]]@(1))
    Check (-not (Assert-Archive $cached $spec)) 'Truncated cache rejected'
    Check (@(Get-ChildItem -LiteralPath $dir -Filter '*.download').Count -eq 0) 'Cache faults never start download'

    $dir = New-Case 'manifest-refusal'
    foreach ($item in @(@('url', 'http://www.python.org/fixture.zip'), @('url', 'https://example.invalid/fixture.zip'),
                       @('sha256', 'A' * 64), @('sha256', 'short'), @('bytes', 0), @('bytes', 67108865))) {
        $bad = [pscustomobject]@{url = 'https://www.python.org/fixture.zip'; bytes = 100; sha256 = 'a' * 64; prefix = ''}
        $bad.($item[0]) = $item[1]
        Throws { Get-VerifiedArchive $dir $bad } 'manifest is invalid'
    }
    Check (@(Get-ChildItem -LiteralPath $dir).Count -eq 0) 'Invalid manifest creates no files'

    $dir = New-Case 'python-extraction'
    $zip = Join-Path $dir 'python.zip'
    Make-Zip $zip @(@('python.exe', 'python'), @('python313.dll', 'dll'), @('python313._pth', "python313.zip`n."))
    $destination = Join-Path $dir 'extracted'
    Expand-Runtime $zip $destination (Spec $zip) $false
    Check (@(Get-ChildItem -LiteralPath $destination).Count -eq 3) 'Python flat archive extracted exactly'
    Check ([IO.File]::ReadAllText((Join-Path $destination 'python313._pth')) -ceq "python313.zip`n.") 'Runtime data preserved'
    Throws { Expand-Runtime $zip $destination (Spec $zip) $false } 'already exists|exist'
    Check ([IO.File]::ReadAllText((Join-Path $destination 'python.exe')) -ceq 'python') 'Existing executable not overwritten'

    $dir = New-Case 'node-selection'
    $zip = Join-Path $dir 'node.zip'
    Make-Zip $zip @(@('node-v22/node.exe', 'node'), @('node-v22/LICENSE', 'license'),
                   @('node-v22/node_modules/npm/index.js', 'ignored'), @('node-v22/../not-extracted', 'ignored'))
    $destination = Join-Path $dir 'extracted'
    Expand-Runtime $zip $destination (Spec $zip 'node-v22/') $true
    Check ((@(Get-ChildItem -LiteralPath $destination | Sort-Object Name).Name -join ',') -ceq 'LICENSE,node.exe') 'Only selected Node files copied'
    Check (-not (Test-Path -LiteralPath (Join-Path $dir 'not-extracted'))) 'Ignored nested entry cannot escape'

    foreach ($badName in @('../escape', '/absolute', 'folder/python.exe', 'folder\python.exe', 'C:escape', 'x:stream', '.', '..')) {
        $dir = New-Case 'bad-python-path'
        $zip = Join-Path $dir 'bad.zip'
        Make-Zip $zip @(@('python.exe', 'python'), @($badName, 'bad'))
        Throws { Expand-Runtime $zip (Join-Path $dir 'extracted') (Spec $zip) $false } 'Unexpected or duplicate'
        Check (-not (Test-Path -LiteralPath (Join-Path $dir 'escape'))) 'No traversal output'
    }
    foreach ($names in @(@('python.exe', 'PYTHON.EXE'), @('python.exe', 'python.exe'))) {
        $dir = New-Case 'duplicate'
        $zip = Join-Path $dir 'duplicate.zip'
        Make-Zip $zip @(@($names[0], 'first'), @($names[1], 'second'))
        Throws { Expand-Runtime $zip (Join-Path $dir 'extracted') (Spec $zip) $false } 'duplicate'
        Check ([IO.File]::ReadAllText((Join-Path $dir 'extracted/python.exe')) -ceq 'first') 'Duplicate cannot replace first file'
    }
    $dir = New-Case 'prefix-missing-empty'
    $zip = Join-Path $dir 'bad.zip'
    Make-Zip $zip (, @('elsewhere/node.exe', 'node'))
    Throws { Expand-Runtime $zip (Join-Path $dir 'wrong') (Spec $zip 'node-v22/') $true } 'layout'
    $zip = Join-Path $dir 'missing.zip'
    Make-Zip $zip (, @('stdlib.zip', 'data'))
    Throws { Expand-Runtime $zip (Join-Path $dir 'missing') (Spec $zip) $false } 'executable is missing'
    $zip = Join-Path $dir 'empty.zip'
    Make-Zip $zip (, @('python.exe', ''))
    Throws { Expand-Runtime $zip (Join-Path $dir 'empty') (Spec $zip) $false } 'bounds'

    $dir = New-Case 'declared-extraction-bound'
    $zip = Join-Path $dir 'large.zip'
    Make-Zip $zip (, @('python.exe', 'small-compressed-fixture'))
    $bytes = [IO.File]::ReadAllBytes($zip)
    $found = 0
    for ($i = 0; $i -lt $bytes.Length - 46; $i++) {
        if ($bytes[$i] -eq 0x50 -and $bytes[$i + 1] -eq 0x4b -and $bytes[$i + 2] -eq 1 -and $bytes[$i + 3] -eq 2) {
            [Array]::Copy([BitConverter]::GetBytes([uint32]167772161), 0, $bytes, $i + 24, 4)
            $found++
        }
    }
    Check ($found -eq 1) 'Patched exact central-directory size, no large decompression'
    [IO.File]::WriteAllBytes($zip, $bytes)
    Throws { Expand-Runtime $zip (Join-Path $dir 'large') (Spec $zip) $false } 'bounds'
    Check (@(Get-ChildItem -LiteralPath (Join-Path $dir 'large')).Count -eq 0) 'Oversized entry rejected before extraction'

    $dir = New-Case 'false-declared-length'
    $zip = Join-Path $dir 'false-length.zip'
    Make-Zip $zip (, @('python.exe', ('x' * 4096)))
    $bytes = [IO.File]::ReadAllBytes($zip)
    for ($i = 0; $i -lt $bytes.Length - 46; $i++) {
        if ($bytes[$i] -eq 0x50 -and $bytes[$i + 1] -eq 0x4b -and $bytes[$i + 2] -eq 1 -and $bytes[$i + 3] -eq 2) {
            [Array]::Copy([BitConverter]::GetBytes([uint32]1), 0, $bytes, $i + 24, 4)
        }
    }
    [IO.File]::WriteAllBytes($zip, $bytes)
    Throws { Expand-Runtime $zip (Join-Path $dir 'short') (Spec $zip) $false } 'length|size|bounds|corrupt'
    Check ((Get-Item -LiteralPath (Join-Path $dir 'short/python.exe')).Length -eq 0) 'Inflated bytes rejected before the first oversized write'
    for ($i = 0; $i -lt $bytes.Length - 46; $i++) {
        if ($bytes[$i] -eq 0x50 -and $bytes[$i + 1] -eq 0x4b -and $bytes[$i + 2] -eq 1 -and $bytes[$i + 3] -eq 2) {
            [Array]::Copy([BitConverter]::GetBytes([uint32]8192), 0, $bytes, $i + 24, 4)
        }
    }
    [IO.File]::WriteAllBytes($zip, $bytes)
    Throws { Expand-Runtime $zip (Join-Path $dir 'truncated') (Spec $zip) $false } 'length|size|bounds|corrupt'

    # Replace only download/extraction boundaries for Start-OpenKeylight. Execute
    # an actual local Node binary with a tiny fixture JS; no installer or device.
    $script:FixtureNode = if ($NodeExe) { (Resolve-Path -LiteralPath $NodeExe).Path } else { (Get-Command node.exe).Source }
    $script:Downloads = 0
    $script:Expansions = 0
    $script:ExtractFailure = $false
    $script:MissingNode = $false
    function Get-VerifiedArchive([string]$Cache, $Spec) { $script:Downloads++; return 'synthetic.zip' }
    function Expand-Runtime([string]$Archive, [string]$Destination, $Spec, [bool]$NodeOnly) {
        $script:Expansions++
        [IO.Directory]::CreateDirectory($Destination) | Out-Null
        if ($script:ExtractFailure) { throw 'Injected extraction failure' }
        if ($NodeOnly -and -not $script:MissingNode) {
            [IO.File]::Copy($script:FixtureNode, (Join-Path $Destination 'node.exe'), $false)
        }
        if (-not $NodeOnly) { [IO.File]::WriteAllText((Join-Path $Destination 'python.exe'), 'never executed') }
    }
    $dir = New-Case 'launch'
    $env:LOCALAPPDATA = Join-Path $dir 'local app data'
    $env:PROCESSOR_ARCHITECTURE = 'AMD64'
    $env:PROCESSOR_ARCHITEW6432 = ''
    $root = Join-Path $dir 'release with spaces'
    foreach ($sub in @('installer', 'tools', 'firmware')) { [IO.Directory]::CreateDirectory((Join-Path $root $sub)) | Out-Null }
    [IO.File]::WriteAllText((Join-Path $root 'tools/stock_migration.py'), '# never executed')
    [IO.File]::WriteAllText((Join-Path $root 'firmware/bundle.json'), '{}')
    [IO.File]::WriteAllText((Join-Path $root 'runtimes.json'), '{"format":1,"platform":"win-x64","node":{},"python":{}}')
    [IO.File]::WriteAllText((Join-Path $root 'installer/cli.js'), 'require("node:fs").writeFileSync(process.env.OKL_TEST_CAPTURE,JSON.stringify(process.argv.slice(2)));process.exit(Number(process.env.OKL_TEST_EXIT||0));')
    $env:OKL_TEST_CAPTURE = Join-Path $dir 'argv.json'
    $env:OKL_TEST_EXIT = '0'
    Start-OpenKeylight $root @('--mode', 'value with spaces', 'punctuation&literal')
    $received = Get-Content -LiteralPath $env:OKL_TEST_CAPTURE -Raw | ConvertFrom-Json
    Check ($received.Count -eq 9) ('Exact fixed and user argument count: ' + ($received | ConvertTo-Json -Compress))
    Check ($received[0] -ceq '--root' -and $received[1] -ceq $root) 'Root passed as one argument'
    Check ($received[2] -ceq '--bundle' -and $received[3] -ceq (Join-Path $root 'firmware/bundle.json')) 'Bundle binding preserved'
    Check ($received[4] -ceq '--python' -and $received[5].EndsWith('\python\python.exe')) 'Isolated Python path passed'
    Check (($received[6..8] -join '|') -ceq '--mode|value with spaces|punctuation&literal') 'User arguments remain literal'
    Check ($script:Downloads -eq 2 -and $script:Expansions -eq 2) 'One preparation per runtime'
    No-Sessions
    $env:OKL_TEST_EXIT = '7'
    Throws { Start-OpenKeylight $root @() } 'installer stopped \(exit 7\)'
    No-Sessions
    $script:ExtractFailure = $true
    Throws { Start-OpenKeylight $root @() } 'Injected extraction failure'
    No-Sessions
    $script:ExtractFailure = $false
    $script:MissingNode = $true
    Throws { Start-OpenKeylight $root @() } 'not recognized|cannot find|does not exist'
    No-Sessions
    $script:MissingNode = $false
    $calls = $script:Downloads
    $env:PROCESSOR_ARCHITECTURE = 'ARM64'
    Throws { Start-OpenKeylight $root @() } 'Windows x64'
    Check ($script:Downloads -eq $calls) 'Unsupported architecture rejected before downloads'
    $env:PROCESSOR_ARCHITECTURE = 'AMD64'
    Remove-Item -LiteralPath (Join-Path $root 'firmware/bundle.json')
    Throws { Start-OpenKeylight $root @() } 'Extract the entire release'
    Check ($script:Downloads -eq $calls) 'Incomplete release rejected before downloads'

    Write-Output ('LAUNCHER_TEST_RESULT ' + (@{checks = $script:Checks; cases = $script:Cases; result = 'pass'; network_requests = 0} | ConvertTo-Json -Compress))
}
catch { Write-Output $_.ScriptStackTrace; throw }
finally {
    $env:LOCALAPPDATA = $oldLocal
    $env:PROCESSOR_ARCHITECTURE = $oldArchitecture
    $env:PROCESSOR_ARCHITEW6432 = $oldArchitecture32
    $env:OKL_TEST_EXIT = $oldExit
    $env:OKL_TEST_CAPTURE = $oldCapture
    $resolved = [IO.Path]::GetFullPath($script:Base)
    $parent = [IO.Path]::GetFullPath([IO.Path]::GetTempPath()).TrimEnd('\') + '\'
    if (-not $resolved.StartsWith($parent, [StringComparison]::OrdinalIgnoreCase) -or
        [IO.Path]::GetFileName($resolved) -notmatch '^okl-launcher-test-[a-f0-9]{32}$') { throw 'Unexpected test cleanup path' }
    if (Test-Path -LiteralPath $resolved) { Remove-Item -LiteralPath $resolved -Recurse -Force }
}

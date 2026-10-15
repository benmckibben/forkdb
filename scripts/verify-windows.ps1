<#
.SYNOPSIS
  Checks the forkdb packages on this machine: the Python wheel and the npm tarball, as a user would install them.

.DESCRIPTION
  1. Gets the packages: from -ArtifactsDir, or with the GitHub CLI from the latest successful "Release artifacts" run of the repository.
  2. Python: finds a Python whose sqlite3 can load extensions, installs the wheel into a fresh virtual environment, runs test\smoke_py.py
     (load, branch, write, isolation, snapshot) and test\verify_install.py (durable vs nosync flushes, compaction, a second process refused,
     a hard kill of a process that holds a store).
  3. Node: installs the tarball into an empty directory and runs test\smoke_node.js with node:sqlite.
  4. Prints a summary to paste back. Exit status 0 only if every step passed.

  Nothing is installed globally and nothing outside a temporary directory is changed. Needs: Git (to have this repository), Python 3.9 or later,
  Node 24 or later (skip with -SkipNode), and either the GitHub CLI logged in ("gh auth login") or the artifacts downloaded by hand.

.EXAMPLE
  powershell -ExecutionPolicy Bypass -File .\scripts\verify-windows.ps1

.EXAMPLE
  powershell -ExecutionPolicy Bypass -File .\scripts\verify-windows.ps1 -ArtifactsDir C:\Users\me\Downloads\forkdb-artifacts
#>
[CmdletBinding()]
param(
    [string]$ArtifactsDir,      # a folder that contains the wheel (*win_amd64*.whl) and the tarball (sqlite-forkdb-*.tgz), anywhere below it
    [string]$RunId,             # a specific "Release artifacts" run to download from (default: the latest successful one)
    [string]$Repo = 'benmckibben/forkdb',
    [switch]$SkipPython,
    [switch]$SkipNode,
    [switch]$KeepWorkDir        # keep the temporary directory (logs, venv, downloaded packages) instead of deleting it
)

$ErrorActionPreference = 'Continue'
$onWindows = ($PSVersionTable.PSVersion.Major -lt 6) -or $IsWindows
$repoRoot = Split-Path -Parent $PSScriptRoot
$work = Join-Path ([IO.Path]::GetTempPath()) ('forkdb-verify-' + [guid]::NewGuid().ToString('N').Substring(0, 8))
New-Item -ItemType Directory -Path $work | Out-Null
$logFile = Join-Path $work 'verify.log'
$results = New-Object System.Collections.ArrayList
$facts = New-Object System.Collections.ArrayList

function Log([string]$msg) { Write-Host $msg; Add-Content -Path $logFile -Value $msg }
function Tag([bool]$ok) { if ($ok) { return 'PASS' } else { return 'FAIL' } }
function Tail([string]$detail) { if ($detail) { return ' - ' + $detail } else { return '' } }
function Record([string]$name, [bool]$ok, [string]$detail) {
    [void]$results.Add([pscustomobject]@{ Name = $name; Ok = $ok; Detail = $detail })
    Log ('[' + (Tag $ok) + '] ' + $name + (Tail $detail))
}
function Fact([string]$k, [string]$v) { [void]$facts.Add($k + ': ' + $v); Log ('      ' + $k + ': ' + $v) }

# Runs a program, shows and logs everything it prints, returns @{ Code; Output }. EnvVars: variables to set for the program (a $null value removes it).
function Invoke-Native {
    param([string]$Exe, [string[]]$Arguments, [string]$WorkDir, [hashtable]$EnvVars)
    $saved = @{}
    if ($EnvVars) { foreach ($k in @($EnvVars.Keys)) { $saved[$k] = [Environment]::GetEnvironmentVariable($k); [Environment]::SetEnvironmentVariable($k, $EnvVars[$k]) } }
    $pushed = $false
    if ($WorkDir) { Push-Location $WorkDir; $pushed = $true }
    $out = @()
    $code = -1
    try {
        $out = @(& $Exe @Arguments 2>&1 | ForEach-Object { $_.ToString() })
        $code = $LASTEXITCODE
    } catch {
        $out = @('could not run ' + $Exe + ': ' + $_.Exception.Message)
        $code = -1
    } finally {
        if ($pushed) { Pop-Location }
        foreach ($k in @($saved.Keys)) { [Environment]::SetEnvironmentVariable($k, $saved[$k]) }
    }
    foreach ($line in $out) { Log ('    | ' + $line) }
    return @{ Code = $code; Output = $out }
}

# A path below the repository, with the separators of this platform (the script is written with backslashes, and is dry-run on Linux).
function RepoPath([string]$rel) { return Join-Path $repoRoot $rel.Replace('\', [string][IO.Path]::DirectorySeparatorChar) }

function Find-Files([string]$dir, [string]$filter) {
    return @(Get-ChildItem -Path $dir -Recurse -File -Filter $filter -ErrorAction SilentlyContinue)
}

function Get-ZipEntryHash([string]$zip, [string]$suffix) {
    Add-Type -AssemblyName System.IO.Compression.FileSystem
    $z = [IO.Compression.ZipFile]::OpenRead($zip)
    try {
        $e = $z.Entries | Where-Object { $_.FullName.EndsWith($suffix) } | Select-Object -First 1
        if (-not $e) { return $null }
        $sha = [Security.Cryptography.SHA256]::Create()
        $s = $e.Open()
        try { $h = $sha.ComputeHash($s) } finally { $s.Dispose() }
        return @{ Name = $e.FullName; Size = $e.Length; Hash = ([BitConverter]::ToString($h) -replace '-', '').ToLower() }
    } finally { $z.Dispose() }
}

function Get-PeMachine([string]$path) {
    $b = [IO.File]::ReadAllBytes($path)
    if ($b.Length -lt 64 -or $b[0] -ne 0x4D -or $b[1] -ne 0x5A) { return 'not a PE file' }
    $pe = [BitConverter]::ToInt32($b, 60)
    $m = [BitConverter]::ToUInt16($b, $pe + 4)
    if ($m -eq 0x8664) { return 'x86_64' }
    if ($m -eq 0x14C) { return 'i386' }
    if ($m -eq 0xAA64) { return 'arm64' }
    return ('machine 0x{0:X}' -f $m)
}

Log ('forkdb Windows verification, ' + (Get-Date -Format 'yyyy-MM-dd HH:mm:ss'))
Log ('work directory: ' + $work)
Fact 'PowerShell' ($PSVersionTable.PSVersion.ToString() + ' (' + $PSVersionTable.PSEdition + ')')
try { Fact 'OS' ((Get-CimInstance Win32_OperatingSystem).Caption + ' build ' + [Environment]::OSVersion.Version.ToString()) } catch { Fact 'OS' ([Environment]::OSVersion.VersionString) }
$archName = $env:PROCESSOR_ARCHITECTURE
if (-not $archName) { $archName = (& uname -m) }
Fact 'architecture' $archName
if (-not $onWindows) { Log '      NOTE: this is not Windows; the script is being dry-run, and it will look for the Linux wheel.' }
Fact 'repository' $repoRoot
foreach ($f in @('test\smoke_py.py', 'test\smoke_node.js', 'test\verify_install.py')) {
    if (-not (Test-Path (RepoPath $f))) { Log ('This script must be run from a checkout of the repository: ' + $f + ' is missing.'); exit 2 }
}
try { Fact 'repository commit' ((& git -C $repoRoot rev-parse --short HEAD 2>&1) | Out-String).Trim() } catch { }

# ---------------------------------------------------------------- the packages
Log ''
Log '== 1. The packages'
$artDir = $ArtifactsDir
if (-not $artDir) {
    $gh = Get-Command gh -ErrorAction SilentlyContinue
    if (-not $gh) {
        Log 'The GitHub CLI (gh) is not installed, and no -ArtifactsDir was given. Either install gh and run "gh auth login", or download the artifacts "wheel-windows-x86_64",'
        Log '"npm-tarball" and "windows-x86_64" from the Actions page of a "Release artifacts" run, unzip them into one folder, and pass it as -ArtifactsDir.'
        Record 'get the packages' $false 'no gh and no -ArtifactsDir'
    } else {
        $id = $RunId
        $sha = ''
        if (-not $id) {
            $r = Invoke-Native -Exe 'gh' -Arguments @('run', 'list', '-R', $Repo, '--workflow', 'release-artifacts.yml', '--status', 'success', '--limit', '1', '--json', 'databaseId,headSha,headBranch')
            try { $j = ($r.Output -join [Environment]::NewLine) | ConvertFrom-Json; $id = [string]$j[0].databaseId; $sha = $j[0].headSha + ' on ' + $j[0].headBranch } catch { }
        }
        if ($id) {
            $artDir = Join-Path $work 'artifacts'
            Fact 'artifacts from run' ($id + $(if ($sha) { ' (' + $sha + ')' } else { '' }))
            $r = Invoke-Native -Exe 'gh' -Arguments @('run', 'download', $id, '-R', $Repo, '-D', $artDir)
            if ($r.Code -ne 0) { Record 'download the artifacts' $false ('gh exited with ' + $r.Code + ' (is "gh auth status" ok, with access to the private repository?)'); $artDir = $null }
        } else {
            Record 'find a Release artifacts run' $false 'gh found no successful run (check "gh auth status")'
        }
    }
}
$wheelGlob = '*win_amd64*.whl'
if (-not $onWindows) { $wheelGlob = '*manylinux*x86_64*.whl' }
$wheel = $null
$tgz = $null
if ($artDir) {
    $wheel = Find-Files $artDir $wheelGlob | Select-Object -First 1
    $tgz = Find-Files $artDir 'sqlite-forkdb-*.tgz' | Select-Object -First 1
    $libFileName = 'forkdb.so'
    if ($onWindows) { $libFileName = 'forkdb.dll' }
    $dll = Find-Files $artDir $libFileName | Select-Object -First 1
    $wheelDetail = 'no ' + $wheelGlob + ' under ' + $artDir
    if ($wheel) { $wheelDetail = $wheel.Name }
    $tgzDetail = 'no sqlite-forkdb-*.tgz under ' + $artDir
    if ($tgz) { $tgzDetail = $tgz.Name }
    Record 'the wheel is there' ([bool]$wheel) $wheelDetail
    Record 'the npm tarball is there' ([bool]$tgz) $tgzDetail
    if ($wheel) {
        Fact 'wheel sha256' (Get-FileHash -Algorithm SHA256 -Path $wheel.FullName).Hash.ToLower()
        $libName = 'forkdb/_lib/forkdb.so'
        if ($onWindows) { $libName = 'forkdb/_lib/forkdb.dll' }
        $inner = Get-ZipEntryHash $wheel.FullName $libName
        if ($inner) {
            Fact 'library inside the wheel' ($inner.Name + ', ' + $inner.Size + ' bytes, sha256 ' + $inner.Hash)
            if ($dll) {
                $same = ((Get-FileHash -Algorithm SHA256 -Path $dll.FullName).Hash.ToLower() -eq $inner.Hash)
                $d = ''
                if (-not $same) { $d = ($libFileName + ' from the build has a different hash') }
                Record 'the library in the wheel is the library the build produced' $same $d
            }
        } else { Record 'the wheel contains the library' $false 'no forkdb/_lib/ entry' }
    }
    if ($dll -and $onWindows) {
        $arch = Get-PeMachine $dll.FullName
        Record 'forkdb.dll is a 64-bit x86 Windows library' ($arch -eq 'x86_64') ('PE machine: ' + $arch + ', ' + $dll.Length + ' bytes')
    }
}

# ---------------------------------------------------------------- python
if ($SkipPython) { Log ''; Log '== 2. Python: skipped (-SkipPython)' }
elseif (-not $wheel) { Log ''; Log '== 2. Python: not run, there is no wheel' }
else {
    Log ''
    Log '== 2. Python'
    $candidates = New-Object System.Collections.ArrayList
    if (Get-Command py -ErrorAction SilentlyContinue) {
        [void]$candidates.Add(@('py', '-3'))
        $listed = @(& py -0p 2>&1 | ForEach-Object { $_.ToString() })
        foreach ($line in $listed) { if ($line -match '(\S*[Pp]ython[0-9.]*\.exe)\s*$') { [void]$candidates.Add(@($Matches[1])) } }
    }
    foreach ($n in @('python', 'python3')) { if (Get-Command $n -ErrorAction SilentlyContinue) { [void]$candidates.Add(@($n)) } }
    $probeFile = Join-Path $work 'probe.py'
    Set-Content -Path $probeFile -Encoding ASCII -Value @('import sys, sqlite3', 'c = sqlite3.connect(":memory:")', 'print("PROBE", sys.version_info >= (3, 9), hasattr(c, "enable_load_extension"), sqlite3.sqlite_version, sys.version.split()[0], sys.executable)')
    $python = $null
    foreach ($c in $candidates) {
        $exe = $c[0]
        $pre = @()
        if ($c.Count -gt 1) { $pre = @($c[1..($c.Count - 1)]) }
        $r = Invoke-Native -Exe $exe -Arguments ($pre + @($probeFile))
        $line = $r.Output | Where-Object { $_ -like 'PROBE *' } | Select-Object -First 1
        if ($line) {
            $p = $line.Split(' ')
            if ($p[1] -eq 'True' -and $p[2] -eq 'True') {
                $python = @{ Exe = $exe; Args = $pre }
                Fact 'Python used' ('Python ' + $p[4] + ', SQLite ' + $p[3] + ', ' + ($p[5..($p.Count - 1)] -join ' '))
                break
            }
            Log ('      not usable: ' + $exe + ' (Python >= 3.9: ' + $p[1] + ', can load SQLite extensions: ' + $p[2] + ')')
        }
    }
    if (-not $python) {
        Record 'a Python that can load SQLite extensions' $false 'none found (needs Python 3.9+ whose sqlite3 has enable_load_extension; the python.org installer or "winget install Python.Python.3.12" usually has it)'
    } else {
        Record 'a Python that can load SQLite extensions' $true ''
        $venv = Join-Path $work 'venv'
        $r = Invoke-Native -Exe $python.Exe -Arguments ($python.Args + @('-m', 'venv', $venv))
        $venvRel = 'bin/python'
        if ($onWindows) { $venvRel = 'Scripts\python.exe' }
        $venvPy = Join-Path $venv $venvRel
        Record 'create a virtual environment' (($r.Code -eq 0) -and (Test-Path $venvPy)) ''
        if (Test-Path $venvPy) {
            $r = Invoke-Native -Exe $venvPy -Arguments @('-m', 'pip', 'install', '--disable-pip-version-check', '--quiet', $wheel.FullName)
            Record 'pip install the wheel' ($r.Code -eq 0) ''
            if ($r.Code -eq 0) {
                $clean = Join-Path $work 'pyrun'
                New-Item -ItemType Directory -Path $clean | Out-Null
                $envv = @{ FORKDB_SMOKE_INSTALLED = '1'; FORKDB_EXTENSION_PATH = $null }
                $r = Invoke-Native -Exe $venvPy -Arguments @((RepoPath 'test\smoke_py.py')) -WorkDir $clean -EnvVars $envv
                Record 'Python smoke test (load, branch, write, isolation, snapshot)' (($r.Code -eq 0) -and ($r.Output -contains 'smoke_py OK')) ('exit ' + $r.Code)
                $r = Invoke-Native -Exe $venvPy -Arguments @((RepoPath 'test\verify_install.py')) -WorkDir $clean -EnvVars $envv
                Record 'Python end-to-end (flushes, compaction, second process, hard kill)' (($r.Code -eq 0) -and ($r.Output -contains 'verify_install OK')) ('exit ' + $r.Code)
            }
        }
    }
}

# ---------------------------------------------------------------- node
if ($SkipNode) { Log ''; Log '== 3. Node: skipped (-SkipNode)' }
elseif (-not $tgz) { Log ''; Log '== 3. Node: not run, there is no tarball' }
else {
    Log ''
    Log '== 3. Node'
    $nodeOk = $false
    if (Get-Command node -ErrorAction SilentlyContinue) {
        $nodeProbe = Join-Path $work 'probe.js'
        Set-Content -Path $nodeProbe -Encoding ASCII -Value @('const { DatabaseSync } = require("node:sqlite");', 'console.log("NODEPROBE", process.version, Number(process.versions.node.split(".")[0]) >= 24, typeof DatabaseSync.prototype.loadExtension);')
        $r = Invoke-Native -Exe 'node' -Arguments @('--no-warnings', $nodeProbe)
        $line = $r.Output | Where-Object { $_ -like 'NODEPROBE *' } | Select-Object -First 1
        if ($line) {
            $p = $line.Split(' ')
            Fact 'Node used' ($p[1] + ', node:sqlite loadExtension: ' + $p[3])
            $nodeOk = (($p[2] -eq 'True') -and ($p[3] -eq 'function'))
        }
    }
    $nd = ''
    if (-not $nodeOk) { $nd = 'not found (needs Node 24+; install from nodejs.org, or use -SkipNode)' }
    Record 'Node 24 or later with node:sqlite' $nodeOk $nd
    if ($nodeOk) {
        $consumer = Join-Path $work 'consumer'
        New-Item -ItemType Directory -Path $consumer | Out-Null
        $r = Invoke-Native -Exe 'npm' -Arguments @('init', '-y') -WorkDir $consumer
        $r = Invoke-Native -Exe 'npm' -Arguments @('install', '--no-audit', '--no-fund', $tgz.FullName) -WorkDir $consumer
        Record 'npm install the tarball in an empty directory' ($r.Code -eq 0) ''
        if ($r.Code -eq 0) {
            $r = Invoke-Native -Exe 'node' -Arguments @('--no-warnings', (RepoPath 'test\smoke_node.js')) -WorkDir $consumer -EnvVars @{ FORKDB_SMOKE_INSTALLED = '1'; FORKDB_EXTENSION_PATH = $null }
            Record 'Node smoke test (node:sqlite: load, branch, write, isolation, snapshot)' (($r.Code -eq 0) -and ($r.Output -contains 'smoke_node OK')) ('exit ' + $r.Code)
        }
    }
}

# ---------------------------------------------------------------- summary
Log ''
Log '================ SUMMARY (paste everything from here down) ================'
foreach ($f in $facts) { Log $f }
$failed = @($results | Where-Object { -not $_.Ok })
foreach ($x in $results) { Log ('[' + (Tag $x.Ok) + '] ' + $x.Name + (Tail $x.Detail)) }
Log ('RESULT: ' + ($results.Count - $failed.Count) + ' passed, ' + $failed.Count + ' failed')
Log ('Full log: ' + $logFile)
if ($KeepWorkDir -or $failed.Count -gt 0) { Log ('Kept: ' + $work) } else { Remove-Item -Recurse -Force $work -ErrorAction SilentlyContinue }
if ($failed.Count -gt 0 -or $results.Count -eq 0) { exit 1 }
exit 0

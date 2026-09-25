# Measurement for mcpp#693. TEMPORARY: this branch is never merged.
#
# Questions:
#   Q1  Which process ends with 0xC0000409 in a directory outside the ANSI code
#       page: mcpp.exe itself, the release zip's batch launcher, or the xlings
#       shim that fronts mcpp in an xlings installation?
#   Q2  Is build.ninja written in the process ANSI code page while Ninja reads
#       it as UTF-8? Tested with a directory the ACP CAN represent (Latin-1 on
#       the runner's code page 1252), where the two encodings differ in bytes.
#   Q3  Does a UTF-8 activeCodePage manifest, embedded into a copy of the
#       released mcpp.exe, change Q1 and Q2, and is it enough end to end
#       (clang, lld, ninja, the std module, build.mcpp)?
#
# Every observation is one `READING <id>: ...` line in the log and in the job
# summary. The script fails only when the probe itself cannot run; a platform
# that says no is a reading, not a failure. Source is ASCII only: every
# non-ASCII name is built from code points below.

$ErrorActionPreference = 'Continue'
$ProgressPreference = 'SilentlyContinue'

$Ver     = '2026.9.25.1'
$XlVer   = '2026.9.20.1'
$Here    = Split-Path -Parent $MyInvocation.MyCommand.Path
$Fx      = Join-Path $Here 'fixtures'

function Reading([string]$id, [string]$text) {
    $line = "READING ${id}: $text"
    Write-Host $line
    if ($env:GITHUB_STEP_SUMMARY) {
        Add-Content -LiteralPath $env:GITHUB_STEP_SUMMARY -Value ("    " + $line) -Encoding utf8
    }
}

function Hex32([int]$code) {
    $u = [BitConverter]::ToUInt32([BitConverter]::GetBytes([int]$code), 0)
    return ('0x{0:X8}' -f $u)
}

# Run a program with an explicit working directory (CreateProcessW underneath,
# so the directory reaches the child intact). Returns exit code and output
# decoded as UTF-8 plus the raw bytes.
function Run([string]$exe, [string[]]$argv, [string]$cwd, [int]$timeoutSec = 600, [hashtable]$envx = @{}) {
    $psi = [System.Diagnostics.ProcessStartInfo]::new()
    $psi.FileName = $exe
    foreach ($a in $argv) { [void]$psi.ArgumentList.Add($a) }
    $psi.WorkingDirectory = $cwd
    $psi.UseShellExecute = $false
    $psi.RedirectStandardOutput = $true
    $psi.RedirectStandardError = $true
    foreach ($k in $envx.Keys) { $psi.Environment[$k] = $envx[$k] }
    $r = [ordered]@{ code = $null; out = ''; err = ''; outBytes = [byte[]]@(); timedOut = $false; startError = '' }
    try {
        $p = [System.Diagnostics.Process]::Start($psi)
    } catch {
        $r.startError = $_.Exception.Message
        return $r
    }
    $o = [System.IO.MemoryStream]::new(); $e = [System.IO.MemoryStream]::new()
    $t1 = $p.StandardOutput.BaseStream.CopyToAsync($o)
    $t2 = $p.StandardError.BaseStream.CopyToAsync($e)
    if (-not $p.WaitForExit($timeoutSec * 1000)) {
        $r.timedOut = $true
        try { $p.Kill($true) } catch {}
        $p.WaitForExit()
    }
    [void]$t1.Wait(10000); [void]$t2.Wait(10000)
    $r.code = $p.ExitCode
    $r.outBytes = $o.ToArray()
    $r.out = [System.Text.Encoding]::UTF8.GetString($r.outBytes)
    $r.err = [System.Text.Encoding]::UTF8.GetString($e.ToArray())
    return $r
}

function OneLine([string]$s, [int]$max = 160) {
    $t = ($s -replace "`r", '' -split "`n" | Where-Object { $_ -ne '' } | Select-Object -First 1)
    if ($null -eq $t) { return '(no output)' }
    if ($t.Length -gt $max) { $t = $t.Substring(0, $max) + '...' }
    return $t
}

function Tail([string]$s, [int]$n = 12) {
    return (($s -replace "`r", '') -split "`n" | Where-Object { $_ -ne '' } | Select-Object -Last $n) -join ' | '
}

function Download([string]$url, [string]$dest) {
    for ($i = 1; $i -le 4; $i++) {
        try { Invoke-WebRequest -Uri $url -OutFile $dest -UseBasicParsing; return $true }
        catch { Write-Host "download attempt $i failed: $($_.Exception.Message)"; Start-Sleep -Seconds (5 * $i) }
    }
    return $false
}

# Bytes around the first occurrence of an ASCII anchor, and counts of the
# candidate encodings of the directory name, in one file.
$L1 = [System.Text.Encoding]::GetEncoding(28591)   # byte-transparent view
function EncodingReport([string]$file, [string]$anchor, [hashtable]$needles) {
    if (-not (Test-Path -LiteralPath $file)) { return "(no $([IO.Path]::GetFileName($file)))" }
    $bytes = [System.IO.File]::ReadAllBytes($file)
    $s = $L1.GetString($bytes)
    $parts = @()
    foreach ($k in $needles.Keys) {
        $n = $L1.GetString($needles[$k])
        $c = 0; $i = 0
        while (($i = $s.IndexOf($n, $i, [StringComparison]::Ordinal)) -ge 0) { $c++; $i += $n.Length }
        $parts += "$k=$c"
    }
    $at = $s.IndexOf($anchor, [StringComparison]::Ordinal)
    $hex = '(anchor absent)'
    if ($at -ge 0) {
        $len = [Math]::Min(20, $bytes.Length - $at)
        $hex = ($bytes[$at..($at + $len - 1)] | ForEach-Object { '{0:X2}' -f $_ }) -join ' '
    }
    return ("{0} bytes; {1}; first '{2}' -> {3}" -f $bytes.Length, ($parts -join ' '), $anchor, $hex)
}

# ---------------------------------------------------------------------------
# 0. The machine
# ---------------------------------------------------------------------------
$nls = Get-ItemProperty 'HKLM:\SYSTEM\CurrentControlSet\Control\Nls\CodePage'
$os  = Get-CimInstance Win32_OperatingSystem
Add-Type -Namespace W -Name K -MemberDefinition '[DllImport("kernel32.dll")] public static extern uint GetACP();'
Reading 'env.acp' "system ACP=$($nls.ACP) OEMCP=$($nls.OEMCP); GetACP() in pwsh=$([W.K]::GetACP())"
Reading 'env.os' "$($os.Caption) build $($os.BuildNumber)"
if ($nls.ACP -eq '65001') {
    Reading 'env.invalid' 'the system ACP is UTF-8; every reading below carries no evidence about #693'
}

# Directory names, from code points.
$e    = [string][char]0x00E9                                    # e-acute, in cp1252
$cjk  = [string][char]0x6D4B + [string][char]0x8BD5              # two CJK ideographs, not in cp1252
$tube = [char]::ConvertFromUtf32(0x1F9EA)                        # U+1F9EA, not in any ANSI code page
$Dirs = [ordered]@{
    ascii  = 'C:\w\ascii'
    latin1 = "C:\w\caf$e"
    nonacp = "C:\w\repro-$cjk-$tube"
}
foreach ($d in $Dirs.Values) { New-Item -ItemType Directory -Force -Path $d | Out-Null }
Reading 'env.dirs' (($Dirs.GetEnumerator() | ForEach-Object { "$($_.Key)=" + (($_.Value.ToCharArray() | ForEach-Object { if ([int]$_ -lt 128) { $_ } else { 'U+{0:X4}' -f [int]$_ } }) -join '') }) -join '; ')

# Needles: the directory name as bytes, per candidate encoding.
$utf8 = [System.Text.UTF8Encoding]::new($false)
$cp1252 = [System.Text.Encoding]::GetEncoding(1252)
$NeedleLatin1 = @{ utf8 = $utf8.GetBytes("caf$e"); acp = $cp1252.GetBytes("caf$e") }
$NeedleNonAcp = @{ utf8 = $utf8.GetBytes("repro-$cjk"); qmarks = [byte[]][char[]]'repro-??' }

# ---------------------------------------------------------------------------
# 1. The released mcpp (zip), a UTF-8-manifest copy of it, and xlings
# ---------------------------------------------------------------------------
New-Item -ItemType Directory -Force -Path C:\dl, C:\m, C:\p\bin, C:\xl, C:\pin, C:\dumps | Out-Null
$zip = "C:\dl\mcpp-$Ver.zip"
if (-not (Download "https://github.com/mcpp-community/mcpp/releases/download/v$Ver/mcpp-$Ver-windows-x86_64.zip" $zip)) {
    Write-Error 'cannot download the mcpp release'; exit 1
}
Expand-Archive -LiteralPath $zip -DestinationPath C:\m -Force
$Root   = "C:\m\mcpp-$Ver-windows-x86_64"
$Rel    = Join-Path $Root 'bin\mcpp.exe'
$Bat    = Join-Path $Root 'mcpp.bat'
$VendXl = Join-Path $Root 'registry\bin\xlings.exe'
Reading 'zip.layout' ((Get-ChildItem -LiteralPath $Root -Recurse -File | ForEach-Object { $_.FullName.Substring($Root.Length + 1) }) -join ', ')

# WER local dumps, so a fast-fail leaves its code and module behind.
foreach ($img in 'mcpp.exe', 'xlings.exe', 'cmd.exe') {
    $k = "HKLM:\SOFTWARE\Microsoft\Windows\Windows Error Reporting\LocalDumps\$img"
    New-Item -Path $k -Force | Out-Null
    New-ItemProperty -Path $k -Name DumpFolder -Value 'C:\dumps' -PropertyType ExpandString -Force | Out-Null
    New-ItemProperty -Path $k -Name DumpType -Value 1 -PropertyType DWord -Force | Out-Null
}

# The UTF-8 activeCodePage copy, embedded with the SDK's mt.exe.
$mt = Get-ChildItem "${env:ProgramFiles(x86)}\Windows Kits\10\bin" -Recurse -Filter mt.exe -ErrorAction SilentlyContinue |
      Where-Object { $_.FullName -match '\\x64\\' } | Sort-Object FullName -Descending | Select-Object -First 1
$Patched = 'C:\p\bin\mcpp.exe'
Copy-Item -LiteralPath $Rel -Destination $Patched -Force
@'
<?xml version="1.0" encoding="UTF-8" standalone="yes"?>
<assembly manifestVersion="1.0" xmlns="urn:schemas-microsoft-com:asm.v1">
  <application xmlns="urn:schemas-microsoft-com:asm.v3">
    <windowsSettings>
      <activeCodePage xmlns="http://schemas.microsoft.com/SMI/2019/WindowsSettings">UTF-8</activeCodePage>
    </windowsSettings>
  </application>
</assembly>
'@ | Set-Content -LiteralPath C:\p\utf8.manifest -Encoding utf8
if ($mt) {
    $orig = Run $mt.FullName @('-nologo', "-inputresource:$Rel;#1", '-out:C:\p\orig.manifest') 'C:\p'
    Reading 'mt.release-has-manifest' ("exit=$(Hex32 $orig.code); " + (OneLine ($orig.out + $orig.err)))
    $emb = Run $mt.FullName @('-nologo', '-manifest', 'C:\p\utf8.manifest', "-outputresource:$Patched;#1") 'C:\p'
    $chk = Run $mt.FullName @('-nologo', "-inputresource:$Patched;#1", '-out:C:\p\check.manifest') 'C:\p'
    $has = (Test-Path C:\p\check.manifest) -and ((Get-Content -Raw C:\p\check.manifest) -match 'activeCodePage')
    Reading 'mt.patched' "embed exit=$(Hex32 $emb.code); read-back exit=$(Hex32 $chk.code); activeCodePage present=$has"
} else {
    Reading 'mt.missing' 'no mt.exe under Windows Kits; the patched leg is skipped'
    $Patched = $null
}

# xlings, installed the way a user installs it, so that `mcpp` resolves to its shim.
$xlZip = "C:\dl\xlings-$XlVer.zip"
$Shim = $null; $XlExe = $null
if (Download "https://github.com/openxlings/xlings/releases/download/v$XlVer/xlings-$XlVer-windows-x86_64.zip" $xlZip) {
    Expand-Archive -LiteralPath $xlZip -DestinationPath C:\xl -Force
    $boot = Get-ChildItem C:\xl -Recurse -Filter xlings.exe | Select-Object -First 1
    $env:XLINGS_NON_INTERACTIVE = '1'
    $si = Run $boot.FullName @('self', 'install') 'C:\xl' 900
    Reading 'xlings.self-install' "exit=$(Hex32 $si.code); $(Tail ($si.out + $si.err) 3)"
    $XlExe = "$env:USERPROFILE\.xlings\subos\default\bin\xlings.exe"
    Set-Content -LiteralPath C:\pin\.xlings.json -Value ('{ "workspace": { "mcpp": "' + $Ver + '" } }') -Encoding ascii
    $inst = Run $XlExe @('install', '-y', '-u', '-g') 'C:\pin' 1500
    Reading 'xlings.install-mcpp' "exit=$(Hex32 $inst.code); $(Tail ($inst.out + $inst.err) 3)"
    $cand = "$env:USERPROFILE\.xlings\subos\default\bin\mcpp.exe"
    if (Test-Path -LiteralPath $cand) { $Shim = $cand }
    Reading 'xlings.shim' "shim=$Shim"
}

# ---------------------------------------------------------------------------
# 2. Q1: `--version` in each directory, through each entry point
# ---------------------------------------------------------------------------
$Entry = [ordered]@{ release = $Rel }
if ($Patched) { $Entry['patched'] = $Patched }
if ($Shim)    { $Entry['xlings-shim'] = $Shim }
if ($XlExe)   { $Entry['xlings'] = $XlExe }
foreach ($dk in $Dirs.Keys) {
    $d = $Dirs[$dk]
    foreach ($ek in $Entry.Keys) {
        $r = Run $Entry[$ek] @('--version') $d 120
        Reading "q1.$dk.$ek" "exit=$(Hex32 $r.code) out=$(OneLine ($r.out + $r.err))"
    }
    $b = Run "$env:SystemRoot\System32\cmd.exe" @('/d', '/c', $Bat, '--version') $d 120
    Reading "q1.$dk.release-bat" "exit=$(Hex32 $b.code) out=$(OneLine ($b.out + $b.err))"
}
Start-Sleep -Seconds 5
$ev = Get-WinEvent -FilterHashtable @{ LogName = 'Application'; Id = 1000 } -ErrorAction SilentlyContinue |
      Where-Object { $_.Message -match 'mcpp|xlings' } | Select-Object -First 8
foreach ($x in $ev) {
    $m = ($x.Message -replace "`r", '' -split "`n" | Where-Object { $_ -match 'Faulting application name|Faulting module name|Exception code|Fault offset|Faulting application path' }) -join ' / '
    Reading 'q1.wer-1000' $m
}
$ev2 = Get-WinEvent -FilterHashtable @{ LogName = 'Application'; Id = 1001 } -ErrorAction SilentlyContinue |
       Where-Object { $_.Message -match 'mcpp|xlings' } | Select-Object -First 4
foreach ($x in $ev2) {
    Reading 'q1.wer-1001' (($x.Message -replace "`r", '' -split "`n" | Where-Object { $_ -match '^P[0-9]+:|Event Name' }) -join ' / ')
}
$dumps = Get-ChildItem C:\dumps -File -ErrorAction SilentlyContinue
Reading 'q1.dumps' (($dumps | ForEach-Object { "$($_.Name) ($($_.Length) B)" }) -join ', ')
$cdb = Get-ChildItem "${env:ProgramFiles(x86)}\Windows Kits\10\Debuggers\x64\cdb.exe" -ErrorAction SilentlyContinue | Select-Object -First 1
if ($cdb -and $dumps) {
    $dmp = $dumps | Select-Object -First 1
    $a = Run $cdb.FullName @('-z', $dmp.FullName, '-c', '.exr -1; kc 30; q') 'C:\dumps' 600
    Write-Host "----- cdb on $($dmp.Name) -----"
    Write-Host $a.out
    Reading 'q1.cdb' (Tail $a.out 30)
}

# ---------------------------------------------------------------------------
# 3. Q2 and Q3: build the fixtures in each directory with each binary
# ---------------------------------------------------------------------------
$env:MCPP_HOME = 'C:\mh'
$env:MCPP_VENDORED_XLINGS = $VendXl
$cfg = Run $Rel @('self', 'config', '--mirror', 'GLOBAL') 'C:\w\ascii' 300
Reading 'setup.mirror' "exit=$(Hex32 $cfg.code)"

$Builders = [ordered]@{ release = $Rel }
if ($Patched) { $Builders['patched'] = $Patched }
$first = $true
foreach ($dk in $Dirs.Keys) {
    foreach ($bk in $Builders.Keys) {
        foreach ($fx in 'hello', 'bmn', 'bmw') {
            $proj = Join-Path $Dirs[$dk] "$bk-$fx"
            Copy-Item -LiteralPath (Join-Path $Fx $fx) -Destination $proj -Recurse -Force
            $t = if ($first) { 2400 } else { 900 }
            $first = $false
            $sw = [System.Diagnostics.Stopwatch]::StartNew()
            $r = Run $Builders[$bk] @('build') $proj $t
            $secs = [int]$sw.Elapsed.TotalSeconds
            $id = "q2.$dk.$bk.$fx"
            $exeOut = ''
            $exe = Get-ChildItem -LiteralPath (Join-Path $proj 'target') -Recurse -Filter probe.exe -ErrorAction SilentlyContinue |
                   Where-Object { $_.FullName -match '\\bin\\' } | Select-Object -First 1
            if ($exe) {
                $x = Run $exe.FullName @() $proj 60
                $exeOut = " run: exit=$(Hex32 $x.code) $(OneLine ($x.out + $x.err))"
            }
            Reading $id "exit=$(Hex32 $r.code) time=${secs}s timedOut=$($r.timedOut)$exeOut"
            if ($r.code -ne 0 -or $r.timedOut) {
                Reading "$id.tail" (Tail ($r.out + $r.err) 14)
            }
            $ninja = Get-ChildItem -LiteralPath (Join-Path $proj 'target') -Recurse -Filter build.ninja -ErrorAction SilentlyContinue | Select-Object -First 1
            $cdbj  = Join-Path $proj 'compile_commands.json'
            if ($dk -eq 'latin1') {
                if ($ninja) { Reading "$id.build-ninja" (EncodingReport $ninja.FullName 'caf' $NeedleLatin1) }
                Reading "$id.cdb" (EncodingReport $cdbj 'caf' $NeedleLatin1)
            } elseif ($dk -eq 'nonacp') {
                if ($ninja) { Reading "$id.build-ninja" (EncodingReport $ninja.FullName 'repro-' $NeedleNonAcp) }
                Reading "$id.cdb" (EncodingReport $cdbj 'repro-' $NeedleNonAcp)
            }
            if ($fx -ne 'hello') {
                $gen = Get-ChildItem -LiteralPath (Join-Path $proj 'target') -Recurse -Filter gen.cpp -ErrorAction SilentlyContinue | Select-Object -First 1
                Reading "$id.generated" ("gen.cpp " + $(if ($gen) { 'written under target' } else { 'absent under target' }))
            }
        }
    }
}

# The Ninja mcpp runs, and the encoding it declares.
$nj = Get-ChildItem C:\mh -Recurse -Filter ninja.exe -ErrorAction SilentlyContinue | Select-Object -First 1
if ($nj) {
    $wc = Run $nj.FullName @('-t', 'wincodepage') 'C:\w\ascii' 60
    $vv = Run $nj.FullName @('--version') 'C:\w\ascii' 60
    Reading 'ninja' "$($nj.FullName.Replace('C:\mh\', '')) version=$(OneLine $vv.out) wincodepage=$(OneLine ($wc.out + $wc.err))"
}
Write-Host 'measurement complete'
exit 0

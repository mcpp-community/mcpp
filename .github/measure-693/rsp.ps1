# Measurement for mcpp#693, round 5. TEMPORARY: this branch is never merged.
#
# Question:
#   R1  In which encodings do the MSVC tools read a response file whose content
#       is not ASCII? mcpp's compile, link and archive edges on Windows pass
#       their arguments through response files that Ninja writes with the bytes
#       of build.ninja, which are UTF-8 under the code page mcpp declares. The
#       earlier rounds produced no response file with non-ASCII content.
#
# Each case writes one response file in one encoding and runs the tool on it.
# Directories: an ASCII control, a name inside code page 1252 (U+00E9) and a
# name outside it (U+6D4B U+8BD5). Encodings: UTF-8 without a BOM, UTF-8 with
# a BOM, UTF-16LE with a BOM, and code page 1252. The last case runs the same
# UTF-8-with-BOM content through Ninja's `rspfile_content`, as mcpp would write
# it.
#
# Every observation is one `READING <id>: ...` line. Source is ASCII only.

$ErrorActionPreference = 'Continue'
$ProgressPreference = 'SilentlyContinue'
[System.Text.Encoding]::RegisterProvider([System.Text.CodePagesEncodingProvider]::Instance)

function Reading([string]$id, [string]$text) {
    $line = "READING ${id}: $text"
    Write-Host $line
    if ($env:GITHUB_STEP_SUMMARY) {
        Add-Content -LiteralPath $env:GITHUB_STEP_SUMMARY -Value ("    " + $line) -Encoding utf8
    }
}

function Run([string]$exe, [string[]]$argv, [string]$cwd) {
    $psi = [System.Diagnostics.ProcessStartInfo]::new()
    $psi.FileName = $exe
    foreach ($a in $argv) { [void]$psi.ArgumentList.Add($a) }
    $psi.WorkingDirectory = $cwd
    $psi.UseShellExecute = $false
    $psi.RedirectStandardOutput = $true
    $psi.RedirectStandardError = $true
    $p = [System.Diagnostics.Process]::Start($psi)
    $o = $p.StandardOutput.ReadToEndAsync(); $e = $p.StandardError.ReadToEndAsync()
    $p.WaitForExit()
    return @{ code = $p.ExitCode; out = ($o.Result + $e.Result) }
}

function FirstLine([string]$s) {
    $t = ($s -replace "`r", '' -split "`n" | Where-Object { $_ -match '\S' } | Select-Object -First 1)
    if ($null -eq $t) { return '(no output)' }
    if ($t.Length -gt 150) { $t = $t.Substring(0, 150) + '...' }
    return $t
}

$Utf8 = [System.Text.UTF8Encoding]::new($false)
$Ansi = [System.Text.Encoding]::GetEncoding(1252)
function Bytes([string]$text, [string]$enc) {
    switch ($enc) {
        'u8'    { return $Utf8.GetBytes($text) }
        'u8bom' { return [byte[]](@(0xEF, 0xBB, 0xBF) + $Utf8.GetBytes($text)) }
        'u16'   { return [byte[]](@(0xFF, 0xFE) + [System.Text.Encoding]::Unicode.GetBytes($text)) }
        'ansi'  { return $Ansi.GetBytes($text) }
    }
}

# ---------------------------------------------------------------------------
# The Visual Studio environment

$vswhere = Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio\Installer\vswhere.exe'
$vs = & $vswhere -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
Import-Module (Join-Path $vs 'Common7\Tools\Microsoft.VisualStudio.DevShell.dll')
Enter-VsDevShell -VsInstallPath $vs -SkipAutomaticLocation -DevCmdArguments '-arch=x64 -host_arch=x64' | Out-Null
$acp = (Get-ItemProperty 'HKLM:\SYSTEM\CurrentControlSet\Control\Nls\CodePage').ACP
$cl = (Get-Command cl.exe).Source
$ninja = Get-Command ninja.exe -ErrorAction SilentlyContinue
Reading 'env' "acp=$acp cl=$cl ninja=$(if ($ninja) { $ninja.Source } else { 'absent' })"
$ver = Run $cl @() $env:TEMP
Reading 'env.cl' (FirstLine $ver.out)

$Root = 'C:\m'
New-Item -ItemType Directory -Force -Path (Join-Path $Root 'a') | Out-Null
$A = Join-Path $Root 'a'
[System.IO.File]::WriteAllText((Join-Path $A 't.c'), "#include `"h.h`"`nint main(void) { return H_OK - 1; }`n")
[System.IO.File]::WriteAllText((Join-Path $A 't0.c'), "int main(void) { return 0; }`n")
$r = Run $cl @('/nologo', '/c', 't0.c', '/Fot0.obj') $A
Reading 'setup.t0' "exit=$($r.code)"

$Dirs = [ordered]@{
    ascii = 'plain'
    cafe  = 'caf' + [char]0x00E9
    cjk   = [string][char]0x6D4B + [char]0x8BD5
}
foreach ($k in $Dirs.Keys) {
    $d = Join-Path $Root $Dirs[$k]
    New-Item -ItemType Directory -Force -Path (Join-Path $d 'inc'), (Join-Path $d 'src') | Out-Null
    [System.IO.File]::WriteAllText((Join-Path $d 'inc\h.h'), "#define H_OK 1`n")
    [System.IO.File]::WriteAllText((Join-Path $d 'src\s.c'), "int s(void) { return 0; }`n")
}

# ---------------------------------------------------------------------------
# R1: one response file per tool, directory and encoding

foreach ($k in $Dirs.Keys) {
    $d = Join-Path $Root $Dirs[$k]
    foreach ($enc in @('u8', 'u8bom', 'u16', 'ansi')) {
        if ($enc -eq 'ansi' -and $k -eq 'cjk') { continue }   # not representable
        $cases = [ordered]@{
            'cl-include' = @{ exe = 'cl.exe';   text = "/nologo /c /I`"$d\inc`" /Fo`"$A\t-$k-$enc.obj`" `"$A\t.c`"`r`n";          out = "$A\t-$k-$enc.obj" }
            'cl-source'  = @{ exe = 'cl.exe';   text = "/nologo /c `"$d\src\s.c`" /Fo`"$A\s-$k-$enc.obj`"`r`n";                 out = "$A\s-$k-$enc.obj" }
            'link-out'   = @{ exe = 'link.exe'; text = "/nologo /OUT:`"$d\o-$enc.exe`" `"$A\t0.obj`"`r`n";                         out = "$d\o-$enc.exe" }
            'lib-out'    = @{ exe = 'lib.exe';  text = "/nologo /OUT:`"$d\l-$enc.lib`" `"$A\t0.obj`"`r`n";                         out = "$d\l-$enc.lib" }
        }
        foreach ($c in $cases.Keys) {
            $rsp = Join-Path $A "$c-$k-$enc.rsp"
            [System.IO.File]::WriteAllBytes($rsp, (Bytes $cases[$c].text $enc))
            $r = Run $cases[$c].exe @("@$rsp") $A
            $made = Test-Path -LiteralPath $cases[$c].out
            $note = if ($r.code -eq 0) { '' } else { ' | ' + (FirstLine $r.out) }
            Reading "R1.$c.$k.$enc" "exit=$($r.code) output=$made$note"
        }
    }
}

# ---------------------------------------------------------------------------
# R1n: the same UTF-8-with-BOM content, written by Ninja from `rspfile_content`

if (-not $ninja) {
    $zip = Join-Path $env:TEMP 'ninja.zip'
    Invoke-WebRequest -Uri 'https://github.com/ninja-build/ninja/releases/download/v1.12.1/ninja-win.zip' -OutFile $zip -UseBasicParsing
    Expand-Archive -LiteralPath $zip -DestinationPath (Join-Path $env:TEMP 'ninja') -Force
    $ninjaExe = Join-Path $env:TEMP 'ninja\ninja.exe'
} else { $ninjaExe = $ninja.Source }
$wcp = Run $ninjaExe @('-t', 'wincodepage') $A
Reading 'R1n.wincodepage' (FirstLine $wcp.out)
foreach ($k in $Dirs.Keys) {
    foreach ($bom in @('none', 'bom')) {
        $d = Join-Path $Root $Dirs[$k]
        $b = Join-Path $Root "n-$k-$bom"
        New-Item -ItemType Directory -Force -Path $b | Out-Null
        $prefix = if ($bom -eq 'bom') { [string][char]0xFEFF } else { '' }
        $text = "ninja_required_version = 1.11`n" +
                "rule cc`n  command = cl.exe @`$out.rsp`n  rspfile = `$out.rsp`n" +
                "  rspfile_content = $prefix/nologo /c /I`"$($d -replace ':', '$$:')\inc`" `$in /Fo`$out`n" +
                "build t.obj: cc $($A -replace ':', '$$:')\t.c`n"
        [System.IO.File]::WriteAllBytes((Join-Path $b 'build.ninja'), $Utf8.GetBytes($text))
        $r = Run $ninjaExe @('-C', $b) $Root
        $note = if ($r.code -eq 0) { '' } else { ' | ' + (FirstLine ($r.out -replace '(?m)^\[.*\]\s*', '')) }
        Reading "R1n.$k.$bom" "exit=$($r.code) output=$(Test-Path -LiteralPath (Join-Path $b 't.obj'))$note"
    }
}

# ---------------------------------------------------------------------------
# R2: where rc.exe and windres look for a file named by a resource statement.
# The script sits in res/, the file beside it, and the tool runs from another
# directory with /I (or -I) naming the project root, as mcpp runs it.

$P = Join-Path $Root 'rcproj'
New-Item -ItemType Directory -Force -Path (Join-Path $P 'res'), (Join-Path $P 'out') | Out-Null
[System.IO.File]::WriteAllText((Join-Path $P 'res\m.manifest'), "<?xml version=`"1.0`"?><assembly xmlns=`"urn:schemas-microsoft-com:asm.v1`" manifestVersion=`"1.0`"/>`n")
[System.IO.File]::WriteAllText((Join-Path $P 'res\beside.rc'), "1 24 `"m.manifest`"`n")
[System.IO.File]::WriteAllText((Join-Path $P 'res\rooted.rc'), "1 24 `"res/m.manifest`"`n")
$O = Join-Path $P 'out'
foreach ($f in @('beside', 'rooted')) {
    $r = Run 'rc.exe' @('/nologo', '/C', '65001', "/I$P", '/fo', "$O\$f.res", "$P\res\$f.rc") $O
    Reading "R2.rc.$f" "exit=$($r.code)$(if ($r.code -ne 0) { ' | ' + (FirstLine $r.out) })"
}
$windres = Get-Command windres.exe -ErrorAction SilentlyContinue
if ($windres) {
    foreach ($f in @('beside', 'rooted')) {
        $r = Run $windres.Source @('-O', 'coff', '--codepage=65001', "-I$P", '-o', "$O\$f.o", "$P\res\$f.rc") $O
        Reading "R2.windres.$f" "exit=$($r.code)$(if ($r.code -ne 0) { ' | ' + (FirstLine $r.out) })"
    }
} else { Reading 'R2.windres' 'absent' }

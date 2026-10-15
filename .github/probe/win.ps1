# O3 (Windows): mcpp $MV with its managed llvm (and any xim payload the
# upstream home also has at the same version) replaced by a junction to the
# upstream home's payload; and a deep MCPP_HOME and project path.
# Prints observations; never asserts.
$ErrorActionPreference = 'Continue'
$XV = '2026.10.15.4'; $MV = '2026.10.11.1'
$T = 'C:\p'; New-Item -ItemType Directory -Force -Path $T, "$T\home" | Out-Null
$S = $env:GITHUB_STEP_SUMMARY
function Say($t) { Write-Host $t; Add-Content -Path $S -Value $t }
function Obs($title, [scriptblock]$cmd, $lines = 25) {
  $out = & $cmd 2>&1 | Out-String; $rc = $LASTEXITCODE
  Write-Host "== $title (exit $rc)"; Write-Host $out
  Add-Content -Path $S -Value "`n**$title** (exit $rc)`n``````"
  Add-Content -Path $S -Value (($out -split "`n") | Select-Object -Last $lines)
  Add-Content -Path $S -Value '```'
}
foreach ($v in 'XLINGS_ACTIVE_SUBOS','XLINGS_SUBOS_MODE','XLINGS_BIN','XLINGS_PROJECT_DIR','XLINGS_HOME','MCPP_VENDORED_XLINGS') {
  Remove-Item "Env:$v" -ErrorAction SilentlyContinue }

Say "### host"
Obs 'LongPathsEnabled' { Get-ItemProperty 'HKLM:\SYSTEM\CurrentControlSet\Control\FileSystem' -Name LongPathsEnabled }

Set-Location $T
Invoke-WebRequest "https://github.com/openxlings/xlings/releases/download/v$XV/xlings-$XV-windows-x86_64.zip" -OutFile xl.zip
Expand-Archive xl.zip -DestinationPath $T -Force
$UP = "$T\xlings-$XV-windows-x86_64"
$XL = "$UP\bin\xlings.exe"
function X { $env:XLINGS_HOME = $UP; & $XL @args; $c = $LASTEXITCODE; Remove-Item Env:XLINGS_HOME; $global:LASTEXITCODE = $c }

Say "### O3: upstream home (xlings $XV), llvm 23.1.3"
Obs 'xlings --version' { X --version }
Obs 'self init' { X self init }
Obs 'update' { X update }
Obs 'install xim:llvm@23.1.3' { X install -y xim:llvm@23.1.3 } 15
Obs 'upstream xim payloads' { Get-ChildItem "$UP\data\xpkgs" | ForEach-Object { "$($_.Name): " + ((Get-ChildItem $_.FullName -Name) -join ' ') } }

Invoke-WebRequest "https://github.com/mcpp-community/mcpp/releases/download/v$MV/mcpp-$MV-windows-x86_64.zip" -OutFile mc.zip
Expand-Archive mc.zip -DestinationPath $T -Force
$M = (Get-ChildItem -Recurse -Path "$T\mcpp-$MV-windows-x86_64" -Filter mcpp.exe | Where-Object { $_.Directory.Name -eq 'bin' } | Select-Object -First 1).FullName
Say "mcpp: $M"

function Run-Demo($label, $mh, $work) {
  $env:MCPP_HOME = $mh
  New-Item -ItemType Directory -Force -Path $work | Out-Null
  Set-Location $work
  Obs "$label : mcpp toolchain install llvm 23.1.3" { & $M toolchain install llvm 23.1.3 } 10
  Obs "$label : mcpp toolchain default llvm@23.1.3" { & $M toolchain default llvm@23.1.3 }
  Obs "$label : mcpp new demo" { & $M new demo }
  Set-Location "$work\demo"
  Obs "$label : build" { & $M build } 15
  Obs "$label : run" { & $M run }
}

Say "### O3: junctions"
$H = "$T\mh"; $W = "$T\w"
Run-Demo 'baseline' $H $W
$reg = "$H\registry\data\xpkgs"
Obs 'mcpp registry xim payloads' { Get-ChildItem $reg | ForEach-Object { "$($_.Name): " + ((Get-ChildItem $_.FullName -Name) -join ' ') } }
function Digest($d) { $f = Get-ChildItem -Recurse -File -Force -Path $d -ErrorAction SilentlyContinue; "$($f.Count) files, $(($f | Measure-Object Length -Sum).Sum) bytes" }
function Link-All($withMarker) {
  $linked = @()
  foreach ($pkg in Get-ChildItem "$UP\data\xpkgs" -Directory) {
    foreach ($ver in Get-ChildItem $pkg.FullName -Directory) {
      $mine = Join-Path $reg (Join-Path $pkg.Name $ver.Name)
      if (Test-Path $mine) {
        $it = Get-Item $mine -Force
        if ($it.LinkType) { cmd /c rmdir "$mine" | Out-Null } else { Remove-Item -Recurse -Force "$mine.own" -ErrorAction SilentlyContinue; Rename-Item $mine "$mine.own" }
        if ($withMarker) { New-Item -ItemType File -Force -Path (Join-Path $ver.FullName '.mcpp_ok') | Out-Null }
        cmd /c mklink /J "$mine" "$($ver.FullName)" | Out-Null
        $linked += "$($pkg.Name)/$($ver.Name)"
      }
    }
  }
  Say "linked by junction (marker=$withMarker): $($linked -join ', ')"
}
$UL = "$UP\data\xpkgs\xim-x-llvm\23.1.3"
$RLW = "$reg\xim-x-llvm\23.1.3"

Say "#### round A: junction, no marker (mcpp reinstalls) -- is the upstream deleted through the junction?"
Say "upstream before: $(Digest $UL)"
Link-All $false
Set-Location "$W\demo"; Remove-Item -Recurse -Force target -ErrorAction SilentlyContinue
Obs 'A: build -v' { & $M -v build } 20
Obs 'A: the entry afterwards' { Get-Item $RLW -Force | Select-Object FullName, LinkType, Target | Format-List }
Say "upstream after A: $(Digest $UL)"

Say "#### round B: junction, marker present (mcpp uses the borrowed payload)"
Link-All $true
Say "upstream before B: $(Digest $UL)"
Set-Location "$W\demo"; Remove-Item -Recurse -Force target -ErrorAction SilentlyContinue
Obs 'B: build -v' { & $M -v build } 40
Obs 'B: the entry afterwards' { Get-Item $RLW -Force | Select-Object FullName, LinkType, Target | Format-List }
Obs 'B: run' { & $M run }
Obs 'B: clang++ resource dir through the junction' { & "$RLW\bin\clang++.exe" -print-resource-dir }
Say "upstream after B: $(Digest $UL)"

Say "### O3: deep MCPP_HOME and project path, LongPathsEnabled = 0"
Set-ItemProperty 'HKLM:\SYSTEM\CurrentControlSet\Control\FileSystem' -Name LongPathsEnabled -Value 0
Obs 'LongPathsEnabled now' { Get-ItemProperty 'HKLM:\SYSTEM\CurrentControlSet\Control\FileSystem' -Name LongPathsEnabled }
$deep = 'C:\d\' + ((1..7 | ForEach-Object { 'segment-of-a-deep-path-' + $_ }) -join '\')
Say "MCPP_HOME length: $($deep.Length + 3)"
Run-Demo 'deep' "$deep\mh" "$deep\w"
$longest = Get-ChildItem -Recurse -Force -Path "$deep" -ErrorAction SilentlyContinue | ForEach-Object { $_.FullName.Length } | Measure-Object -Maximum
Say "longest path under the deep root: $($longest.Maximum)"
$tl = Get-ChildItem -Recurse -Force -Path "$deep\w\demo\target" -ErrorAction SilentlyContinue | Sort-Object { $_.FullName.Length } -Descending | Select-Object -First 3 | ForEach-Object { "$($_.FullName.Length) $($_.FullName)" }
Obs 'longest paths in target' { $tl }

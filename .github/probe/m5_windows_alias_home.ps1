# M5 (Windows): the homes of mcpp and of xlings on a substituted drive.
. "$PSScriptRoot\common.ps1"
$mcpp = Get-Mcpp
$stage = Join-Path $T 'stage5'
New-Item -ItemType Directory -Force $stage | Out-Null
$Z = Get-Alias-Drive $stage
Write-Host "alias drive: $Z -> $stage"

Say 'M5: mcpp with its home and the project on the alias'
$env:MCPP_HOME = "$Z\mh"
New-Hello "$Z\proj"
Push-Location "$Z\proj"
Show { & $mcpp self env } 30
Show { & $mcpp toolchain list } 30
$log = Join-Path $T 'm5-build.log'
& $mcpp build -v *> $log
Write-Host "[build exit $LASTEXITCODE]"
Get-Content $log | Select-String -Pattern 'warning|error|degraded|root' | Select-Object -First 30 | ForEach-Object { $_.Line.Substring(0, [Math]::Min(400, $_.Line.Length)) }
Get-Content $log | Select-Object -Last 12 | ForEach-Object { $_.Substring(0, [Math]::Min(400, $_.Length)) }
$phys = $stage
Write-Host "lines naming the alias: $((Select-String -LiteralPath $log -SimpleMatch "$Z\" | Measure-Object).Count + (Select-String -LiteralPath $log -SimpleMatch "$Z/" | Measure-Object).Count)"
Write-Host "lines naming the physical directory: $((Select-String -LiteralPath $log -SimpleMatch $phys | Measure-Object).Count + (Select-String -LiteralPath $log -SimpleMatch ($phys -replace '\\','/') | Measure-Object).Count)"
Select-String -LiteralPath $log -SimpleMatch $phys | Select-Object -First 5 | ForEach-Object { $_.Line.Substring(0, [Math]::Min(500, $_.Line.Length)) }
Select-String -LiteralPath $log -SimpleMatch ($phys -replace '\\','/') | Select-Object -First 5 | ForEach-Object { $_.Line.Substring(0, [Math]::Min(500, $_.Line.Length)) }
Pop-Location
$exe = Get-ChildItem -LiteralPath "$Z\proj\target" -Recurse -Filter hello.exe -ErrorAction SilentlyContinue | Select-Object -First 1
if ($exe) { Show { & $exe.FullName } }

Say 'M5: the released xlings with its home on the alias'
$xzip = Join-Path $T 'xlings.zip'
Invoke-WebRequest -Uri "https://github.com/openxlings/xlings/releases/download/v$($env:XLINGS_VERSION)/xlings-$($env:XLINGS_VERSION)-windows-x86_64.zip" -OutFile $xzip
Expand-Archive -LiteralPath $xzip -DestinationPath (Join-Path $T 'xlings') -Force
$x = (Get-ChildItem -LiteralPath (Join-Path $T 'xlings') -Recurse -Filter xlings.exe | Select-Object -First 1).FullName
Write-Host "xlings: $x"
$env:XLINGS_AGENT_MODE = '1'
$env:XLINGS_HOME = "$Z\xh"
New-Item -ItemType Directory -Force "$Z\xh" | Out-Null
Show { & $x --version }
Show { & $x install ninja } 30
Show { & $x self doctor } 30
Write-Host '-- which spelling the home''s own files carry'
$files = Get-ChildItem -LiteralPath (Join-Path $stage 'xh') -Recurse -File -ErrorAction SilentlyContinue | Where-Object { $_.Length -lt 2MB }
$p1 = ($files | Select-String -SimpleMatch $phys -List -ErrorAction SilentlyContinue)
$p2 = ($files | Select-String -SimpleMatch ($phys -replace '\\','/') -List -ErrorAction SilentlyContinue)
$a1 = ($files | Select-String -SimpleMatch "$Z\xh" -List -ErrorAction SilentlyContinue)
$a2 = ($files | Select-String -SimpleMatch "$Z/xh" -List -ErrorAction SilentlyContinue)
Write-Host "files naming the physical directory: $(@($p1).Count + @($p2).Count)"
@($p1) + @($p2) | Select-Object -First 10 | ForEach-Object { Write-Host "  $($_.Path)" }
Write-Host "files naming the alias: $(@($a1).Count + @($a2).Count)"
@($a1) + @($a2) | Select-Object -First 10 | ForEach-Object { Write-Host "  $($_.Path)" }

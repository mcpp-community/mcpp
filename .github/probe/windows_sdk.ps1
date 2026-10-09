# O6 / D17 / D18 probe: what the published mcpp does when the Windows SDK is not
# under the conventional "C:\Program Files (x86)\Windows Kits\10" path, as on a
# machine whose SDK was installed to another drive. Prints; asserts nothing.
$ErrorActionPreference = 'Continue'
$PSNativeCommandUseErrorActionPreference = $false
$v = $env:MCPP_VERSION
$root = 'C:\probe'
$logs = "$root\logs"
New-Item -ItemType Directory -Force $logs | Out-Null
$summary = $env:GITHUB_STEP_SUMMARY
function Say($t) { Write-Host $t; Add-Content -Path $summary -Value $t }

Say "## windows sdk probe (mcpp $v)"
$zip = "$root\mcpp.zip"
Invoke-WebRequest "https://github.com/mcpp-community/mcpp/releases/download/v$v/mcpp-$v-windows-x86_64.zip" -OutFile $zip
Expand-Archive $zip -DestinationPath "$root\dist" -Force
$mcpp = (Get-ChildItem "$root\dist" -Recurse -Filter mcpp.exe | Select-Object -First 1).FullName
Say "- mcpp: ``$mcpp``"
Say "- WindowsSdkDir in env: ``$env:WindowsSdkDir``  VSINSTALLDIR: ``$env:VSINSTALLDIR``"

function Run-Case($name, $mcppHome, $dir, [string[]]$cmdArgs) {
    $env:MCPP_HOME = $mcppHome
    New-Item -ItemType Directory -Force $mcppHome | Out-Null
    Push-Location $dir
    $log = "$logs\$name.log"
    & $mcpp @cmdArgs *>&1 | Tee-Object -FilePath $log | Out-Host
    $rc = $LASTEXITCODE
    Pop-Location
    Say "### $name (exit $rc)"
    Say '```'
    Get-Content $log | Select-String -Pattern 'First run|Default|Resolved|warning|error|note|hint|Visual Studio|SDK|MinGW|mingw|Finished|toolchain' |
        Select-Object -First 40 | ForEach-Object { Say $_.Line }
    Say '```'
    $cfg = Join-Path $mcppHome 'config.toml'
    if (Test-Path $cfg) {
        Say "config.toml after ${name}:"
        Say '```'
        Get-Content $cfg | Select-String -Pattern 'toolchain|target' | ForEach-Object { Say $_.Line }
        Say '```'
    }
    return $rc
}

function New-Hello($name, [string]$toolchainLine) {
    $dir = "$root\$name"
    New-Item -ItemType Directory -Force "$dir\src" | Out-Null
    $toml = "[package]`nname = `"$name`"`nversion = `"0.1.0`"`n"
    if ($toolchainLine) { $toml += "`n[toolchain]`n$toolchainLine`n" }
    $toml += "`n[targets.$name]`nkind = `"bin`"`nmain = `"src/main.cpp`"`n"
    Set-Content -Path "$dir\mcpp.toml" -Value $toml -NoNewline
    Set-Content -Path "$dir\src\main.cpp" -Value "import std;`nint main() { std::println(`"hello from {}`", `"$name`"); }`n"
    return $dir
}

$home1 = "$root\home-first-run"
$home2 = "$root\home-llvm"

# B0: control, SDK in its conventional place, llvm declared.
$b0 = New-Hello 'b0_control_llvm' 'windows = "llvm@23.1.3"'
Run-Case 'B0_control_llvm_declared' $home2 $b0 @('build') | Out-Null

# Move the SDK and record its new place where an installer would.
$kits = Join-Path ${env:ProgramFiles(x86)} 'Windows Kits\10'
$moved = 'C:\MovedKits\10'
New-Item -ItemType Directory -Force 'C:\MovedKits' | Out-Null
Say "### moving ``$kits`` -> ``$moved``"
try { Move-Item -Path $kits -Destination $moved -ErrorAction Stop; Say '- moved' }
catch { Say "- Move-Item failed: $($_.Exception.Message)"; cmd /c "move `"$kits`" `"$moved`"" | Out-Host }
Say "- conventional path exists now: $(Test-Path $kits); moved path exists: $(Test-Path $moved)"
foreach ($k in @('HKLM:\SOFTWARE\WOW6432Node\Microsoft\Windows Kits\Installed Roots',
                 'HKLM:\SOFTWARE\Microsoft\Windows Kits\Installed Roots')) {
    if (Test-Path $k) {
        $old = (Get-ItemProperty $k -ErrorAction SilentlyContinue).KitsRoot10
        Set-ItemProperty -Path $k -Name KitsRoot10 -Value "$moved\"
        Say "- ``$k`` KitsRoot10: ``$old`` -> ``$moved\``"
    }
}
foreach ($k in @('HKLM:\SOFTWARE\WOW6432Node\Microsoft\Microsoft SDKs\Windows\v10.0',
                 'HKLM:\SOFTWARE\Microsoft\Microsoft SDKs\Windows\v10.0')) {
    if (Test-Path $k) {
        $old = (Get-ItemProperty $k -ErrorAction SilentlyContinue).InstallationFolder
        Set-ItemProperty -Path $k -Name InstallationFolder -Value "$moved\"
        Say "- ``$k`` InstallationFolder: ``$old`` -> ``$moved\``"
    }
}

# S1: nothing configured anywhere (fresh home, no [toolchain]).
$s1 = New-Hello 's1_nothing_configured' ''
Run-Case 'S1_first_run_nothing_configured' $home1 $s1 @('build') | Out-Null
Run-Case 'S1_self_env' $home1 $s1 @('self', 'env') | Out-Null

# S2: [toolchain] windows = llvm in mcpp.toml (llvm already installed by B0).
$s2 = New-Hello 's2_llvm_declared' 'windows = "llvm@23.1.3"'
$rc = Run-Case 'S2_llvm_declared_in_manifest' $home2 $s2 @('build')
if ($rc -eq 0) {
    $exe = Get-ChildItem "$s2\target" -Recurse -Filter 's2_llvm_declared.exe' | Select-Object -First 1
    if ($exe) { Say "S2 program output: $(& $exe.FullName)" }
}

# S3: nothing in mcpp.toml, --toolchain on the command line.
$s3 = New-Hello 's3_toolchain_flag' ''
Run-Case 'S3_toolchain_flag' $home2 $s3 @('build', '--toolchain', 'llvm@23.1.3') | Out-Null

# S4: S2 again with --verbose, for the SDK lines the plain run hides.
Run-Case 'S4_llvm_declared_verbose' $home2 $s2 @('build', '--verbose') | Out-Null
exit 0

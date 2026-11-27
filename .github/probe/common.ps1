# Shared by the Windows probe scripts. Prints and never asserts.
$ErrorActionPreference = 'Continue'
$PSNativeCommandUseErrorActionPreference = $false
function Say($m) { Write-Host "`n===== $m =====" }
function Show([scriptblock]$b, [int]$last = 0) {
    Write-Host ('$ ' + $b.ToString().Trim())
    $out = & $b 2>&1 | ForEach-Object { "$_" }
    if ($last -gt 0) { $out | Select-Object -Last $last | ForEach-Object { Write-Host $_ } }
    else { $out | ForEach-Object { Write-Host $_ } }
    Write-Host "[exit $LASTEXITCODE]"
}
$T = Join-Path $env:RUNNER_TEMP 'probe'
New-Item -ItemType Directory -Force $T | Out-Null

function Get-Mcpp {
    $zip = Join-Path $T 'mcpp.zip'
    if (-not (Test-Path $zip)) {
        Invoke-WebRequest -Uri "https://github.com/mcpp-community/mcpp/releases/download/v$($env:MCPP_VERSION)/mcpp-windows-x86_64.zip" -OutFile $zip
        Expand-Archive -LiteralPath $zip -DestinationPath (Join-Path $T 'mcpp') -Force
    }
    (Get-ChildItem -LiteralPath (Join-Path $T 'mcpp') -Recurse -Filter mcpp.exe | Select-Object -First 1).FullName
}

function Get-Alias-Drive([string]$target) {
    foreach ($l in 'Z','Y','X','W','V') {
        if (-not (Test-Path "${l}:\")) {
            & subst "${l}:" $target | Out-Null
            if (Test-Path "${l}:\") { return "${l}:" }
        }
    }
    return $null
}

function New-Hello([string]$dir) {
    New-Item -ItemType Directory -Force (Join-Path $dir 'src') | Out-Null
    @"
[package]
name    = "hello"
version = "0.1.0"

[targets.hello]
kind = "bin"
main = "src/main.cpp"
"@ | Set-Content -Encoding utf8 (Join-Path $dir 'mcpp.toml')
    "#include <cstdio>`nint main() { std::puts(`"hello`"); return 0; }`n" | Set-Content -Encoding utf8 (Join-Path $dir 'src\main.cpp')
}

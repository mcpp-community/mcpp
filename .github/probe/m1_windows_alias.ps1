# M1: the image name and the working directory a program reads when mcpp starts
# it from a substituted drive, beside the same program started directly.
. "$PSScriptRoot\common.ps1"
$mcpp = Get-Mcpp
Write-Host "mcpp: $mcpp"
Show { & $mcpp --version }

Say 'a program that reports how it was started'
$spySrc = Join-Path $T 'spy.c'
@'
#include <windows.h>
#include <stdio.h>
int main(void) {
    char image[4096]; char cwd[4096];
    GetModuleFileNameA(NULL, image, sizeof image);
    GetCurrentDirectoryA(sizeof cwd, cwd);
    fprintf(stderr, "[spy] module=%s | cwd=%s | drives=0x%08lx | command=%s\n",
            image, cwd, (unsigned long)GetLogicalDrives(), GetCommandLineA());
    return 86;
}
'@ | Set-Content -Encoding ascii $spySrc
$spy = Join-Path $T 'spy.exe'
$gcc = Get-Command gcc -ErrorAction SilentlyContinue
if ($gcc) {
    Show { & gcc $spySrc -o $spy -static }
} else {
    $vs = & "${env:ProgramFiles(x86)}\Microsoft Visual Studio\Installer\vswhere.exe" -latest -property installationPath
    Show { cmd /c "`"$vs\VC\Auxiliary\Build\vcvars64.bat`" >nul && cl /nologo /MT `"$spySrc`" /Fe:`"$spy`"" }
}
Write-Host "spy built: $(Test-Path $spy)"

$stage = Join-Path $T 'stage'
New-Item -ItemType Directory -Force (Join-Path $stage 'tc\bin') | Out-Null
Copy-Item $spy (Join-Path $stage 'tc\bin\clang++.exe') -Force
Copy-Item $spy (Join-Path $stage 'tc\bin\clang.exe') -Force
$Z = Get-Alias-Drive $stage
Write-Host "alias drive: $Z -> $stage"
Show { & subst }

Say 'M1 control: started directly'
Show { & "$Z\tc\bin\clang++.exe" --version }
Show { & (Join-Path $stage 'tc\bin\clang++.exe') --version }

$env:MCPP_HOME = Join-Path $T 'mh'
New-Hello (Join-Path $stage 'proj')

Say 'M1: started by mcpp, toolchain named on the alias, project on the alias'
$env:MCPP_TOOLCHAIN = "path:$Z/tc"
Push-Location "$Z\proj"
Show { & $mcpp build } 12
Pop-Location

Say 'M1: started by mcpp, toolchain named on the alias, project on the physical path'
Push-Location (Join-Path $stage 'proj')
Show { & $mcpp build } 12
Pop-Location

Say 'M1: started by mcpp, toolchain named by the physical path'
$env:MCPP_TOOLCHAIN = "path:$($stage -replace '\\','/')/tc"
Push-Location "$Z\proj"
Show { & $mcpp build } 12
Pop-Location
Remove-Item Env:MCPP_TOOLCHAIN

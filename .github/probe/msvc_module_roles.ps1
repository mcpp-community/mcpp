# O5 probe: which cl.exe options compile each C++20 module role, in .cpp and
# .cppm/.ixx files, and whether each writes the BMI an importer needs.
# Prints; asserts nothing.
$ErrorActionPreference = 'Continue'
$PSNativeCommandUseErrorActionPreference = $false
$summary = $env:GITHUB_STEP_SUMMARY
function Say($t) { Write-Host $t; Add-Content -Path $summary -Value $t }
$d = 'C:\roles'
New-Item -ItemType Directory -Force $d | Out-Null
Set-Location $d

Set-Content m.ixx        "export module m;`nexport import :api;`nexport int twice(int);`n"
Set-Content api.cppm     "export module m:api;`nexport int answer();`n"
Set-Content api_cpp.cpp  "export module m2:api;`nexport int answer2();`n"
Set-Content impl_part.cpp "module m:impl;`nimport :api;`nint helper() { return answer(); }`n"
Set-Content impl_part.cppm "module m:impl2;`nimport :api;`nint helper2() { return answer(); }`n"
Set-Content iface.cpp    "export module solo;`nexport int solo_value() { return 7; }`n"
Set-Content impl.cpp     "module m;`nint twice(int x) { return 2 * x; }`n"

Say "## cl.exe module roles"
Say '```'
& cl 2>&1 | Select-Object -First 1 | ForEach-Object { Say $_ }
Say '```'

function Try-Cl($label, [string[]]$a) {
    Remove-Item *.obj -ErrorAction SilentlyContinue
    $t0 = Get-Date
    Start-Sleep -Milliseconds 1100
    $out = & cl /nologo /std:c++latest /EHsc /c @a 2>&1
    $rc = $LASTEXITCODE
    $ifc = (Get-ChildItem *.ifc -ErrorAction SilentlyContinue | Where-Object { $_.LastWriteTime -gt $t0 } | ForEach-Object Name) -join ' '
    Say "### $label  (exit $rc)"
    Say "command: ``cl /std:c++latest /c $($a -join ' ')``  — .ifc written: ``$ifc``"
    Say '```'
    $out | Select-Object -First 8 | ForEach-Object { Say "$_" }
    Say '```'
}

# Interface partition in .cppm: needs /interface (cl does not know .cppm).
Try-Cl 'interface partition, .cppm, no option'          @('api.cppm')
Try-Cl 'interface partition, .cppm, /interface /TP'     @('/interface', '/TP', 'api.cppm')
# Primary interface in .ixx.
Try-Cl 'primary interface, .ixx'                        @('m.ixx')
# Implementation partition in .cpp: plain, /internalPartition, /interface.
Try-Cl 'implementation partition, .cpp, no option'      @('impl_part.cpp')
Try-Cl 'implementation partition, .cpp, /internalPartition' @('/internalPartition', 'impl_part.cpp')
Try-Cl 'implementation partition, .cpp, /interface'     @('/interface', 'impl_part.cpp')
# Implementation partition in .cppm.
Try-Cl 'implementation partition, .cppm, /internalPartition /TP' @('/internalPartition', '/TP', 'impl_part.cppm')
Try-Cl 'implementation partition, .cppm, /interface /TP' @('/interface', '/TP', 'impl_part.cppm')
# Interface unit in .cpp.
Try-Cl 'interface unit, .cpp, no option'                @('iface.cpp')
Try-Cl 'interface unit, .cpp, /interface'               @('/interface', 'iface.cpp')
Try-Cl 'interface partition, .cpp, /interface'          @('/interface', 'api_cpp.cpp')
# Implementation unit (`module m;`) in .cpp, with m.ifc present.
Try-Cl 'implementation unit, .cpp, no option'           @('impl.cpp')
exit 0

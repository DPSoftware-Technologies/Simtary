# Makes deps/openssl-msys: OpenSSL headers + DLLs from msys2 UCRT64 (pacman -S mingw-w64-ucrt-x86_64-openssl)
# and MSVC import libraries generated from the DLL exports. The DLLs depend only on Windows + UCRT.
param([string]$Msys = "E:\msys64\ucrt64", [string]$Dst = "$PSScriptRoot\..\..\deps\openssl-msys")
$vs = (Get-ChildItem "C:\Program Files*\Microsoft Visual Studio\*\*\VC\Tools\MSVC\*\bin\Hostx64\x64" | Select-Object -Last 1).FullName
New-Item -ItemType Directory -Force "$Dst\lib", "$Dst\bin", "$Dst\include" | Out-Null
Copy-Item "$Msys\include\openssl" "$Dst\include\" -Recurse -Force
Copy-Item "$Msys\bin\libssl-3-x64.dll", "$Msys\bin\libcrypto-3-x64.dll" "$Dst\bin\" -Force
foreach ($n in 'libssl-3-x64', 'libcrypto-3-x64') {
    $exp = & "$vs\dumpbin.exe" /exports "$Dst\bin\$n.dll" |
        Where-Object { $_ -match '^\s+\d+\s+[0-9A-F]+\s+[0-9A-F]{8}\s+(\S+)' } | ForEach-Object { ($_ -split '\s+')[4] }
    ("LIBRARY $n.dll", "EXPORTS") + ($exp | Where-Object { $_ -and $_ -ne '[NONAME]' }) | Set-Content "$Dst\lib\$n.def" -Encoding ascii
    & "$vs\lib.exe" /nologo /def:"$Dst\lib\$n.def" /machine:x64 /out:"$Dst\lib\$n.lib" | Out-Null
    Copy-Item "$Dst\lib\$n.lib" ("$Dst\lib\" + ($n -replace '-3-x64','') + '.lib') -Force
}


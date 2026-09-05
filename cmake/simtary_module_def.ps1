# Generates the host executable's export list (.def) from what the project module
# actually references.
#
# Why not WINDOWS_EXPORT_ALL_SYMBOLS: the engine archive alone holds ~64,700 public
# symbols and a PE export table caps at 65,535. Exporting the engine wholesale does
# not fit, and would not be wanted if it did - every export is a link-time root that
# survives /OPT:REF.
#
# So the list is derived instead:
#
#   undefined externals in the module's object files
#     INTERSECTED WITH
#   symbols defined by the host (framework objects + the static libraries it links)
#
# Everything else the module references - the CRT, Win32 imports, SDL2, its own
# template instantiations - it resolves through its own link and must NOT come from
# the host. The intersection is exactly the set that has to cross the boundary, and
# for a normal project it is a few thousand names rather than sixty-odd thousand.
#
# Data symbols are exported with the DATA keyword. A module that reads an engine
# global directly, without __declspec(dllimport), then fails to link with a plain
# LNK2019 instead of silently binding to the wrong address - see the warning below.

param(
    [Parameter(Mandatory=$true)][string]$Dumpbin,      # full path to dumpbin.exe
    [Parameter(Mandatory=$true)][string]$UndefinedFrom,# file listing the module's .obj files
    [Parameter(Mandatory=$true)][string]$DefinedFrom,  # file listing host .obj and .lib files
    [Parameter(Mandatory=$true)][string]$Output,       # .def to write
    [string]$LibraryName = ""                          # LIBRARY line; blank for an exe
)

$ErrorActionPreference = 'Stop'

# PE export tables index by 16-bit ordinal. Blowing this is the failure the whole
# derived-list approach exists to avoid, so it is checked rather than discovered.
$ExportLimit = 65535

function Read-PathList ([string]$listFile) {
    if (-not (Test-Path $listFile)) { return @() }
    Get-Content -LiteralPath $listFile |
        ForEach-Object { $_.Trim() } |
        Where-Object { $_ -ne '' -and (Test-Path -LiteralPath $_) }
}

# Public symbols in a static library, straight out of the archive's linker member.
# One dumpbin call for the whole archive, which is what keeps this affordable on a
# 190 MB engine lib.
#
# Cached against the archive's write time, because this script runs on every build
# while the engine archive changes on almost none of them: re-dumping ~64,700 engine
# symbols each time is the difference between a step nobody notices and one everybody
# does.
function Get-LibrarySymbols ([string]$lib, [string]$cacheDir) {
    $stamp = (Get-Item -LiteralPath $lib).LastWriteTimeUtc.Ticks
    $cache = Join-Path $cacheDir ((Split-Path -Leaf $lib) + ".symcache")

    if (Test-Path -LiteralPath $cache) {
        $cached = Get-Content -LiteralPath $cache
        if ($cached.Count -gt 0 -and $cached[0] -eq "$stamp") {
            return $cached[1..($cached.Count - 1)]
        }
    }

    $out = New-Object System.Collections.Generic.List[string]
    $out.Add("$stamp")
    foreach ($line in (& $Dumpbin /NOLOGO /LINKERMEMBER:1 $lib)) {
        # "  16DB964 ?Update@xinput@input@wi@@YAXXZ" - address then a single token.
        # The member header lines ("B7C96D size", "64726 public symbols") carry a
        # second word and are filtered out by requiring exactly one.
        if ($line -match '^\s+[0-9A-Fa-f]+\s+(\S+)\s*$') { $out.Add($Matches[1]) }
    }
    Set-Content -LiteralPath $cache -Value $out -Encoding ascii
    return $out[1..($out.Count - 1)]
}

# Externals in an object file, split by whether the object defines them or needs them.
# dumpbin /SYMBOLS puts the storage class and the section on the left of the "|" and
# the decorated name on the right:
#   008 00000000 UNDEF  notype ()    External     | ?Run@st@@YAHHPEAPEADAEAU...
#
# The left half also says whether the symbol is code or data: dumpbin prints the type
# as "notype ()" for a function and a bare "notype" for a variable. That is taken from
# the REFERENCING object rather than guessed from the mangled name, because the name
# cannot be read reliably - "@@3" appears inside template back-references
# (?Register@SceneManager@@QEAAX...@3@@Z) as often as it appears as the marker for a
# global variable, and calling a function "data" breaks a link that should work.
function Get-ObjectSymbols ([string]$obj) {
    $defined   = New-Object System.Collections.Generic.List[string]
    $undefined = New-Object System.Collections.Generic.List[string]
    $undefinedData = New-Object System.Collections.Generic.List[string]
    foreach ($line in (& $Dumpbin /NOLOGO /SYMBOLS $obj)) {
        $split = $line.IndexOf('|')
        if ($split -lt 0) { continue }
        $left  = $line.Substring(0, $split)
        $name  = $line.Substring($split + 1).Trim()
        if ($name -eq '' -or $left -notmatch '\bExternal\b') { continue }
        # A name can be followed by the demangled form in parentheses; the decorated
        # name is the first token and the only one the linker knows.
        $name = ($name -split '\s+')[0]
        if ($left -match '\bUNDEF\b') {
            $undefined.Add($name)
            if ($left -notmatch '\(\)') { $undefinedData.Add($name) }
        } else {
            $defined.Add($name)
        }
    }
    [PSCustomObject]@{ Defined = $defined; Undefined = $undefined; UndefinedData = $undefinedData }
}

# Same cache treatment for the framework's own object files. There are dozens of them
# and only the ones the developer just edited have changed, so caching turns the
# defined-set scan into a handful of dumpbin calls.
function Get-ObjectDefinedSymbols ([string]$obj, [string]$cacheDir) {
    $stamp = (Get-Item -LiteralPath $obj).LastWriteTimeUtc.Ticks
    # Object files from different targets can share a base name, so the cache key
    # includes a hash of the full path.
    $hash  = [System.BitConverter]::ToString(
        [System.Security.Cryptography.MD5]::Create().ComputeHash(
            [System.Text.Encoding]::UTF8.GetBytes($obj))).Replace('-', '').Substring(0, 12)
    $cache = Join-Path $cacheDir ((Split-Path -Leaf $obj) + ".$hash.symcache")

    if (Test-Path -LiteralPath $cache) {
        $cached = Get-Content -LiteralPath $cache
        if ($cached.Count -gt 0 -and $cached[0] -eq "$stamp") {
            if ($cached.Count -eq 1) { return @() }
            return $cached[1..($cached.Count - 1)]
        }
    }

    $symbols = (Get-ObjectSymbols $obj).Defined
    $lines = New-Object System.Collections.Generic.List[string]
    $lines.Add("$stamp")
    foreach ($sym in $symbols) { $lines.Add($sym) }
    Set-Content -LiteralPath $cache -Value $lines -Encoding ascii
    return $symbols
}

# the host's defined set
$cacheDir = Split-Path -Parent $Output
if ($cacheDir -eq '') { $cacheDir = '.' }
if (-not (Test-Path -LiteralPath $cacheDir)) {
    New-Item -ItemType Directory -Path $cacheDir -Force | Out-Null
}

$defined = New-Object 'System.Collections.Generic.HashSet[string]'
foreach ($path in (Read-PathList $DefinedFrom)) {
    if ($path -like '*.lib') {
        foreach ($sym in (Get-LibrarySymbols $path $cacheDir)) { [void]$defined.Add($sym) }
    } else {
        foreach ($sym in (Get-ObjectDefinedSymbols $path $cacheDir)) { [void]$defined.Add($sym) }
    }
}

# what the module asks the host for
$wanted   = New-Object 'System.Collections.Generic.HashSet[string]'
$dataRefs = New-Object 'System.Collections.Generic.HashSet[string]'
foreach ($obj in (Read-PathList $UndefinedFrom)) {
    $symbols = Get-ObjectSymbols $obj
    foreach ($sym in $symbols.Undefined) {
        # __imp_ references are already an import and are satisfied by whatever import
        # library the module links; re-exporting them from the host would be wrong.
        if ($sym -like '__imp_*') { continue }
        if ($defined.Contains($sym)) { [void]$wanted.Add($sym) }
    }
    foreach ($sym in $symbols.UndefinedData) { [void]$dataRefs.Add($sym) }
}

$names = @($wanted) | Sort-Object
if ($names.Count -gt $ExportLimit) {
    throw "Module boundary needs $($names.Count) exports, over the PE limit of $ExportLimit. " +
          "Narrow what the module reaches into, or move that code into the framework."
}

$dataExports = @($names | Where-Object { $dataRefs.Contains($_) })

# write the .def
$lines = New-Object System.Collections.Generic.List[string]
$lines.Add("; Generated by cmake/simtary_module_def.ps1 - do not edit.")
$lines.Add("; $($names.Count) symbols the project module resolves out of the host.")
if ($LibraryName -ne '') { $lines.Add("LIBRARY $LibraryName") }
$lines.Add("EXPORTS")
foreach ($name in $names) {
    if ($dataRefs.Contains($name)) { $lines.Add("    $name DATA") }
    else                           { $lines.Add("    $name") }
}

$outDir = Split-Path -Parent $Output
if ($outDir -ne '' -and -not (Test-Path -LiteralPath $outDir)) {
    New-Item -ItemType Directory -Path $outDir -Force | Out-Null
}

# Only rewrite on a real change: the .def is a link input, and touching it every
# build would relink the host (and therefore the module) for nothing.
$new = ($lines -join "`r`n") + "`r`n"
$old = if (Test-Path -LiteralPath $Output) { Get-Content -LiteralPath $Output -Raw } else { '' }
if ($new -ne $old) {
    Set-Content -LiteralPath $Output -Value $new -Encoding ascii -NoNewline
}

Write-Host "Module exports: $($names.Count) symbols -> $Output"
if ($dataExports.Count -gt 0) {
    Write-Host "  note: $($dataExports.Count) are global variables, exported DATA."
    Write-Host "  A module reading one of these directly will not link. Reach them"
    Write-Host "  through an accessor function in the framework instead."
}

$ErrorActionPreference = "Stop"

$root = Split-Path -Parent $PSScriptRoot
Set-Location -LiteralPath $root

function Fail([string]$message) {
    Write-Error $message
    exit 1
}

$manifestPath = Join-Path $root "provenance/core-dependencies.json"
if (-not (Test-Path -LiteralPath $manifestPath -PathType Leaf)) {
    Fail "missing provenance manifest: provenance/core-dependencies.json"
}

$manifest = Get-Content -LiteralPath $manifestPath -Raw | ConvertFrom-Json
if ($manifest.schema_version -ne 1) {
    Fail "unsupported provenance manifest schema"
}
if ($null -eq $manifest.compiled_dependencies -or $null -eq $manifest.shipped_data_assets) {
    Fail "manifest must declare compiled_dependencies and shipped_data_assets"
}

foreach ($entry in @($manifest.compiled_dependencies) + @($manifest.shipped_data_assets)) {
    if ([string]::IsNullOrWhiteSpace($entry.path) -or $entry.sha256 -notmatch '^[0-9a-fA-F]{64}$') {
        Fail "every dependency and asset requires a path and SHA-256"
    }

    $candidate = [IO.Path]::GetFullPath((Join-Path $root $entry.path))
    if (-not $candidate.StartsWith($root, [StringComparison]::OrdinalIgnoreCase)) {
        Fail "manifest path escapes repository: $($entry.path)"
    }
    if (-not (Test-Path -LiteralPath $candidate -PathType Leaf)) {
        Fail "manifest file is missing: $($entry.path)"
    }

    $actual = (Get-FileHash -LiteralPath $candidate -Algorithm SHA256).Hash
    if ($actual -ne $entry.sha256) {
        Fail "manifest hash mismatch: $($entry.path)"
    }
}

$dryRun = (& mingw32-make -B -n -f Makefile.blaze test perft-driver blaze 2>&1) -join "`n"
if ($LASTEXITCODE -ne 0) {
    Fail "could not inspect the build dependency graph"
}
if ($dryRun -match '(?i)(^|[\\/])vendor([\\/])' -or $dryRun -match '(?i)stockfish') {
    Fail "build dependency graph crosses the clean-room boundary"
}

$compiledSources = [regex]::Matches($dryRun, '(?i)([^\s"]+\.cpp)') |
    ForEach-Object { $_.Groups[1].Value.Replace('\', '/') } |
    Sort-Object -Unique
foreach ($source in $compiledSources) {
    if ($source -notmatch '^(src/blaze/|tests/|tools/perft_driver\.cpp$)') {
        Fail "unexpected compiled source outside the Blaze boundary: $source"
    }
}

$sourceFiles = Get-ChildItem -LiteralPath (Join-Path $root "src/blaze"), (Join-Path $root "tests") -Recurse -File |
    Where-Object { $_.Extension -in '.h', '.hpp', '.c', '.cc', '.cpp' }
$sourceFiles += Get-Item -LiteralPath (Join-Path $root "tools/perft_driver.cpp")
foreach ($file in $sourceFiles) {
    $content = Get-Content -LiteralPath $file.FullName -Raw
    if ($content -match '(?im)^\s*#\s*include[^\r\n]*(stockfish|vendor[\\/])') {
        Fail "forbidden source include: $($file.FullName)"
    }
}

$forbiddenNetworks = Get-ChildItem -LiteralPath (Join-Path $root "src/blaze") -Recurse -File |
    Where-Object { $_.Name -match '(?i)(\.nnue$|^nn-.*\.bin$|^stockfish.*\.(bin|dat)$)' }
if ($forbiddenNetworks) {
    Fail "forbidden evaluation network inside the Blaze source tree"
}

foreach ($binaryName in "build/blaze/blaze_tests.exe", "build/blaze/perft_driver.exe", "build/blaze/blaze.exe") {
    $binaryPath = Join-Path $root $binaryName
    if (-not (Test-Path -LiteralPath $binaryPath -PathType Leaf)) {
        Fail "expected verification binary is missing: $binaryName"
    }
    $binaryText = [Text.Encoding]::ASCII.GetString([IO.File]::ReadAllBytes($binaryPath))
    if ($binaryText.IndexOf("Stockfish", [StringComparison]::OrdinalIgnoreCase) -ge 0) {
        Fail "forbidden identifier embedded in binary: $binaryName"
    }
}

Write-Output "clean-room verification passed"

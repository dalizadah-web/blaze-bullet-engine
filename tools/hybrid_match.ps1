[CmdletBinding()]
param(
    [string]$Repo = "",
    [string]$WorkflowRef = "codex/bullet-beast",
    [string]$CandidateRef = "codex/bullet-beast",
    [string]$BaselineRef = "4d25363fef79ff2025670e248ed07b3d81747d3a",
    [ValidateRange(2, 1000000)][int]$CloudGames = 400,
    [ValidateRange(2, 1000000)][int]$LocalGames = 400,
    [ValidateRange(1, 20)][int]$CloudShards = 20,
    [ValidateRange(1, 64)][int]$LocalConcurrency = 8,
    [string]$TimeControl = "0.5+0",
    [ValidateRange(1, 2)][int]$Threads = 1,
    [ValidateRange(1, 65536)][int]$HashMb = 16,
    [double]$Elo0 = 0,
    [double]$Elo1 = 5,
    [string]$Output = ""
)

$ErrorActionPreference = "Stop"
$ProjectRoot = Split-Path -Parent $PSScriptRoot
$stamp = Get-Date -Format "yyyyMMdd-HHmmss"
if (-not $Output) { $Output = "artifacts/hybrid/$stamp" }
$OutputRoot = [System.IO.Path]::GetFullPath((Join-Path $ProjectRoot $Output))
$BuildRoot = Join-Path $ProjectRoot "build/hybrid-$stamp"

if (($LocalGames % 2) -ne 0) { throw "LocalGames must be even." }
if (($CloudGames % 2) -ne 0 -or (($CloudGames / 2) % $CloudShards) -ne 0) {
    throw "CloudGames must be even and its pairs must divide across CloudShards."
}

$make = Get-Command mingw32-make -ErrorAction SilentlyContinue
if (-not $make) { $make = Get-Command make -ErrorAction SilentlyContinue }
if (-not $make) { throw "GNU Make is required (mingw32-make or make)." }

$runner = Get-Command cutechess-cli -ErrorAction SilentlyContinue
if ($runner) {
    $runnerPath = $runner.Source
} else {
    $commonDir = git -C $ProjectRoot rev-parse --git-common-dir
    if ($LASTEXITCODE -ne 0) { throw "Cannot locate the Git common directory." }
    if ([System.IO.Path]::IsPathRooted($commonDir)) {
        $commonDir = [System.IO.Path]::GetFullPath($commonDir)
    } else {
        $commonDir = [System.IO.Path]::GetFullPath((Join-Path $ProjectRoot $commonDir))
    }
    $repositoryRoot = Split-Path -Parent $commonDir
    $bundledRunner = Join-Path $repositoryRoot "tools-bin/cutechess-1.5.1/cutechess-1.5.1-win64/cutechess-cli.exe"
    $runnerPath = if (Test-Path -LiteralPath $bundledRunner -PathType Leaf) {
        $bundledRunner
    } else {
        Join-Path $repositoryRoot "cutechess-cli.exe"
    }
}
if (-not (Test-Path -LiteralPath $runnerPath -PathType Leaf)) {
    throw "cutechess-cli was not found. Put cutechess-cli.exe in the main repository root or on PATH."
}

New-Item -ItemType Directory -Force -Path $OutputRoot, $BuildRoot | Out-Null

Write-Host "Dispatching cloud lane: up to $($CloudShards * 2) simultaneous games."
& (Join-Path $PSScriptRoot "cloud_match.ps1") -Action Run -Repo $Repo `
    -WorkflowRef $WorkflowRef -CandidateRef $CandidateRef -BaselineRef $BaselineRef `
    -Games $CloudGames -Shards $CloudShards -TimeControl $TimeControl `
    -Threads $Threads -HashMb $HashMb -Elo0 $Elo0 -Elo1 $Elo1 -CloudOnly
if ($LASTEXITCODE -ne 0) { throw "Cloud lane dispatch failed." }

try {
    $candidateSource = Join-Path $BuildRoot "candidate-src"
    $baselineSource = Join-Path $BuildRoot "baseline-src"
    New-Item -ItemType Directory -Force -Path $candidateSource, $baselineSource | Out-Null
    $candidateTar = Join-Path $BuildRoot "candidate.tar"
    $baselineTar = Join-Path $BuildRoot "baseline.tar"
    git -C $ProjectRoot archive --format=tar --output=$candidateTar $CandidateRef
    if ($LASTEXITCODE -ne 0) { throw "Cannot archive candidate ref $CandidateRef." }
    git -C $ProjectRoot archive --format=tar --output=$baselineTar $BaselineRef
    if ($LASTEXITCODE -ne 0) { throw "Cannot archive baseline ref $BaselineRef." }
    tar -xf $candidateTar -C $candidateSource
    tar -xf $baselineTar -C $baselineSource

    Write-Host "Building frozen local candidate and baseline."
    & $make.Source -C $candidateSource -f Makefile.blaze blaze -j 8
    if ($LASTEXITCODE -ne 0) { throw "Local candidate build failed." }
    & $make.Source -C $baselineSource -f Makefile.blaze blaze -j 8
    if ($LASTEXITCODE -ne 0) { throw "Local baseline build failed." }

    $frozen = Join-Path $OutputRoot "local-frozen"
    New-Item -ItemType Directory -Force -Path $frozen | Out-Null
    $candidateBin = Join-Path $frozen "candidate.exe"
    $baselineBin = Join-Path $frozen "baseline.exe"
    $frozenRunner = Join-Path $frozen "cutechess-cli.exe"
    Copy-Item -LiteralPath (Join-Path $candidateSource "build/blaze/blaze.exe") -Destination $candidateBin
    Copy-Item -LiteralPath (Join-Path $baselineSource "build/blaze/blaze.exe") -Destination $baselineBin
    Copy-Item -LiteralPath $runnerPath -Destination $frozenRunner
    $runnerDirectory = Split-Path -Parent $runnerPath
    Get-ChildItem -LiteralPath $runnerDirectory -File -Filter "*.dll" | ForEach-Object {
        $dependency = $_
        Copy-Item -LiteralPath $dependency.FullName -Destination $frozen
    }

    $candidateCommit = git -C $ProjectRoot rev-parse $CandidateRef
    Write-Host "Running local lane: $LocalConcurrency simultaneous games using up to $($LocalConcurrency * 2) engine threads."
    python -m tools.cloud_match.local_lane `
        --base-spec config/cloud/default-match.json `
        --candidate-ref $CandidateRef --baseline-ref $BaselineRef `
        --candidate $candidateBin --baseline $baselineBin --runner $frozenRunner `
        --output (Join-Path $OutputRoot "local") --games $LocalGames `
        --concurrency $LocalConcurrency --time-control $TimeControl `
        --threads $Threads --hash-mb $HashMb --elo0 $Elo0 --elo1 $Elo1 `
        --source-commit $candidateCommit
    if ($LASTEXITCODE -ne 0) { throw "Local match lane failed." }
}
finally {
    $resolvedBuild = [System.IO.Path]::GetFullPath($BuildRoot)
    $expectedParent = [System.IO.Path]::GetFullPath((Join-Path $ProjectRoot "build"))
    if ($resolvedBuild.StartsWith($expectedParent, [System.StringComparison]::OrdinalIgnoreCase) -and
        (Test-Path -LiteralPath $resolvedBuild)) {
        Remove-Item -LiteralPath $resolvedBuild -Recurse -Force
    }
}

Write-Host "Local lane complete. Waiting for the latest cloud lane."
& (Join-Path $PSScriptRoot "cloud_match.ps1") -Action Watch -Repo $Repo
if ($LASTEXITCODE -ne 0) { throw "Cloud match failed; local evidence remains at $OutputRoot." }
$cloudOutput = Join-Path $OutputRoot "cloud"
& (Join-Path $PSScriptRoot "cloud_match.ps1") -Action Download -Repo $Repo -Output $cloudOutput
if ($LASTEXITCODE -ne 0) { throw "Cloud result download failed." }

$cloudSummary = Get-ChildItem -LiteralPath $cloudOutput -Recurse -Filter summary.json |
    Where-Object { $_.FullName -match "result-" } |
    Select-Object -First 1
if (-not $cloudSummary) { throw "Downloaded artifacts contain no strict cloud summary.json." }
$localSummary = Join-Path $OutputRoot "local/lane-summary.json"
$hybridSummary = Join-Path $OutputRoot "hybrid-summary.json"
python -m tools.cloud_match.combine --lanes $cloudSummary.FullName $localSummary --output $hybridSummary
if ($LASTEXITCODE -ne 0) { throw "Hybrid evidence combination failed." }
Write-Host "Hybrid match complete: $hybridSummary"

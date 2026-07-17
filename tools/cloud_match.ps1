[CmdletBinding()]
param(
    [ValidateSet("Run", "Status", "Watch", "Open", "Download")]
    [string]$Action = "Status",
    [string]$Repo = "",
    [string]$WorkflowRef = "codex/bullet-beast",
    [string]$CandidateRef = "codex/bullet-beast",
    [string]$BaselineRef = "4d25363",
    [ValidateRange(2, 1000000)][int]$Games = 400,
    [ValidateRange(1, 20)][int]$Shards = 20,
    [string]$TimeControl = "0.5+0",
    [ValidateRange(1, 2)][int]$Threads = 1,
    [ValidateRange(1, 65536)][int]$HashMb = 16,
    [double]$Elo0 = 0,
    [double]$Elo1 = 5,
    [long]$RunId = 0,
    [string]$Output = "artifacts/cloud-match"
)

$ErrorActionPreference = "Stop"
$ProjectRoot = Split-Path -Parent $PSScriptRoot

function Get-GitHubCli {
    $command = Get-Command gh -ErrorAction SilentlyContinue
    if (-not $command) {
        $installed = Join-Path $env:ProgramFiles "GitHub CLI/gh.exe"
        if (Test-Path -LiteralPath $installed -PathType Leaf) {
            $command = Get-Item -LiteralPath $installed
        }
    }
    if (-not $command) {
        throw "GitHub CLI is required. Install it with: winget install --id GitHub.cli --exact --source winget"
    }
    & $command.Source auth status 2>$null | Out-Null
    if ($LASTEXITCODE -ne 0) {
        throw "GitHub CLI is not signed in. Run: gh auth login --web"
    }
    return $command.Source
}

function Resolve-Repo([string]$ExplicitRepo) {
    if ($ExplicitRepo) { return $ExplicitRepo }
    $remote = git -C $ProjectRoot remote get-url origin 2>$null
    if ($LASTEXITCODE -ne 0 -or -not $remote) {
        throw "No origin remote exists. Pass -Repo owner/repository or add the GitHub origin."
    }
    if ($remote -match "github\.com[/:]([^/]+)/([^/]+)$") {
        return "$($Matches[1])/$($Matches[2] -replace '\.git$', '')"
    }
    throw "Origin is not a GitHub repository: $remote"
}

function Resolve-RunId([string]$Gh, [string]$Repository, [long]$Requested) {
    if ($Requested -gt 0) { return $Requested }
    $json = & $Gh run list --repo $Repository --workflow cloud-match.yml --limit 1 --json databaseId
    if ($LASTEXITCODE -ne 0) { throw "Could not list workflow runs." }
    $runs = $json | ConvertFrom-Json
    if (-not $runs -or $runs.Count -eq 0) { throw "No cloud match runs exist yet." }
    return [long]$runs[0].databaseId
}

$gh = Get-GitHubCli
$repository = Resolve-Repo $Repo

switch ($Action) {
    "Run" {
        if (($Games % 2) -ne 0) { throw "Games must be even." }
        if ((($Games / 2) % $Shards) -ne 0) {
            throw "The number of game pairs must divide evenly across shards."
        }
        & $gh workflow run cloud-match.yml --repo $repository --ref $WorkflowRef `
            -f "candidate_ref=$CandidateRef" `
            -f "baseline_ref=$BaselineRef" `
            -f "games=$Games" `
            -f "shards=$Shards" `
            -f "time_control=$TimeControl" `
            -f "threads=$Threads" `
            -f "hash_mb=$HashMb" `
            -f "elo0=$Elo0" `
            -f "elo1=$Elo1"
        if ($LASTEXITCODE -ne 0) { throw "Workflow dispatch failed." }
        Write-Host "Dispatched $Games games across $Shards runners (~$($Shards * 2) simultaneous matches)."
        Write-Host "Check it with: powershell -File tools/cloud_match.ps1 -Action Status"
    }
    "Status" {
        & $gh run list --repo $repository --workflow cloud-match.yml --limit 10
        if ($LASTEXITCODE -ne 0) { throw "Could not list workflow runs." }
    }
    "Watch" {
        $id = Resolve-RunId $gh $repository $RunId
        & $gh run watch $id --repo $repository --exit-status
        if ($LASTEXITCODE -ne 0) { throw "Cloud match $id failed." }
    }
    "Open" {
        $id = Resolve-RunId $gh $repository $RunId
        & $gh run view $id --repo $repository --web
        if ($LASTEXITCODE -ne 0) { throw "Could not open cloud match $id." }
    }
    "Download" {
        $id = Resolve-RunId $gh $repository $RunId
        $destination = Join-Path $ProjectRoot $Output
        New-Item -ItemType Directory -Force -Path $destination | Out-Null
        & $gh run download $id --repo $repository --dir $destination
        if ($LASTEXITCODE -ne 0) { throw "Could not download cloud match $id." }
        Write-Host "Downloaded run $id to $destination"
    }
}

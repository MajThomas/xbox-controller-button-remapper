#Requires -Version 5.1
<#
  Publish local Xbox Controller Button Remapper changes to a GitHub fork,
  then publish EXACT files from .\x64\Release\ as GitHub Release assets.

  Usage:
    .\Publish-GitHub.ps1
    .\Publish-GitHub.ps1 -Tag v2.0.0-custom.2
    .\Publish-GitHub.ps1 -Tag v2.0.0-custom.2 -Yes
    .\Publish-GitHub.ps1 -Branch main (only if automatic branch detection is ambiguous)

  This script does NOT modify the local source tree or local config.ini.
  It clones the GitHub fork into a temporary working directory.
#>
[CmdletBinding()]
param(
    [ValidatePattern('^[A-Za-z0-9][A-Za-z0-9._-]{0,79}$')]
    [string]$Tag = 'v2.0.0-custom.1',
    [string]$Branch = '',  # Optional: specify a remote branch if automatic detection is ambiguous
    [switch]$Yes
)

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'
$ProjectRoot = Split-Path -Parent $MyInvocation.MyCommand.Path
$ReleaseDir  = Join-Path $ProjectRoot 'x64\Release'
$UpstreamRepo = 'Adam777Z/xbox-controller-button-remapper'
$RepoName     = 'xbox-controller-button-remapper'
$TempRoot     = Join-Path ([System.IO.Path]::GetTempPath()) ('xbox-github-publish-' + [guid]::NewGuid().ToString('N'))
$Succeeded = $false

function Assert-Command([string]$Name, [string]$InstallHelp) {
    if (-not (Get-Command $Name -ErrorAction SilentlyContinue)) {
        throw "Required command '$Name' was not found. $InstallHelp"
    }
}
function Invoke-Checked([string]$Program, [string[]]$Arguments) {
    & $Program @Arguments
    if ($LASTEXITCODE -ne 0) {
        throw "Command failed (exit=$LASTEXITCODE): $Program $($Arguments -join ' ')"
    }
}
function Test-CommandSuccess([string]$Program, [string[]]$Arguments) {
    # Expected failures (e.g. a fork/tag not existing yet) must not abort the script.
    $previousPreference = $ErrorActionPreference
    $ErrorActionPreference = 'Continue'
    try {
        & $Program @Arguments *> $null
        return ($LASTEXITCODE -eq 0)
    } finally {
        $ErrorActionPreference = $previousPreference
    }
}
function Assert-File([string]$Path) {
    if (-not (Test-Path -LiteralPath $Path -PathType Leaf)) {
        throw "Missing file: $Path"
    }
    if ((Get-Item -LiteralPath $Path).Length -eq 0) {
        throw "File is empty: $Path"
    }
}
function Copy-Verified([string]$Source, [string]$Target) {
    Copy-Item -LiteralPath $Source -Destination $Target -Force
    if ((Get-FileHash -LiteralPath $Source -Algorithm SHA256).Hash -ne
        (Get-FileHash -LiteralPath $Target -Algorithm SHA256).Hash) {
        throw "File copy checksum mismatch: $Source"
    }
}
function Copy-SourceTree([string]$SourceDir, [string]$TargetDir) {
    # Only project source/docs/scripts: never upload x64, SDL build outputs, packages, .vs, .git or secrets.
    $extensions = @('.cpp', '.c', '.h', '.hpp', '.rc', '.ico', '.manifest',
                    '.sln', '.vcxproj', '.filters', '.props', '.targets', '.config',
                    '.md', '.txt', '.ini', '.html', '.cmd', '.ps1')
    foreach ($file in (Get-ChildItem -LiteralPath $SourceDir -File -Force)) {
        if ($extensions -notcontains $file.Extension.ToLowerInvariant()) { continue }
        Copy-Item -LiteralPath $file.FullName -Destination (Join-Path $TargetDir $file.Name) -Force
    }
    $testSource = Join-Path $SourceDir 'tests'
    if (Test-Path -LiteralPath $testSource -PathType Container) {
        $testTarget = Join-Path $TargetDir 'tests'
        New-Item -ItemType Directory -Path $testTarget -Force | Out-Null
        foreach ($file in (Get-ChildItem -LiteralPath $testSource -File -Recurse)) {
            if ($extensions -notcontains $file.Extension.ToLowerInvariant()) { continue }
            $relative = $file.FullName.Substring($testSource.Length).TrimStart('\', '/')
            $target = Join-Path $testTarget $relative
            New-Item -ItemType Directory -Path (Split-Path -Parent $target) -Force | Out-Null
            Copy-Item -LiteralPath $file.FullName -Destination $target -Force
        }
    }
}


function Get-RemoteBranchNames([string]$CloneDir) {
    # Work entirely from git's fetched remote-tracking refs. No GitHub API request.
    $refLines = @(& git -C $CloneDir for-each-ref '--format=%(refname:strip=3)' 'refs/remotes/origin')
    if ($LASTEXITCODE -ne 0) {
        throw 'Git could not read local origin/* remote-tracking branches.'
    }
    $names = @($refLines | Where-Object {
        -not [string]::IsNullOrWhiteSpace($_) -and $_ -cne 'HEAD'
    } | ForEach-Object { $_.Trim() } | Sort-Object -Unique)
    return $names
}
function Resolve-ForkBranch([string]$Repository, [string]$CloneDir, [string]$PreferredBranch) {
    # A fork may have a missing/invalid remote HEAD. Clone can download all
    # objects but leave the local repository without any checked-out branch.
    # Query Git remote-tracking refs, not gh api repos/<owner>/<repo>.
    $available = @(Get-RemoteBranchNames $CloneDir)
    if ($available.Count -eq 0) {
        Write-Host 'No local origin/* branches; fetching fork branches once...' -ForegroundColor Yellow
        Invoke-Checked 'git' @('-C', $CloneDir, 'fetch', 'origin', '+refs/heads/*:refs/remotes/origin/*') | Out-Null
        $available = @(Get-RemoteBranchNames $CloneDir)
    }
    if ($available.Count -eq 0) {
        throw "Fork $Repository has no branches after fetch. Check its GitHub page."
    }

    Write-Host ('Fork remote branches: ' + ($available -join ', '))
    $remoteHead = $null
    $headOutput = @(& git -C $CloneDir symbolic-ref --quiet --short 'refs/remotes/origin/HEAD')
    if ($LASTEXITCODE -eq 0 -and $headOutput.Count -gt 0) {
        $candidateHead = [string]$headOutput[0]
        if ($candidateHead.StartsWith('origin/', [System.StringComparison]::Ordinal)) {
            $remoteHead = $candidateHead.Substring(7)
        }
    }

    $branch = $null
    if (-not [string]::IsNullOrWhiteSpace($PreferredBranch)) {
        if ($available -cnotcontains $PreferredBranch) {
            throw "Requested -Branch '$PreferredBranch' does not exist. Available branches: $($available -join ', ')."
        }
        $branch = $PreferredBranch
    } elseif ($null -ne $remoteHead -and $available -ccontains $remoteHead) {
        $branch = $remoteHead
    } elseif ($available -ccontains 'main') {
        $branch = 'main'
    } elseif ($available -ccontains 'master') {
        $branch = 'master'
    } elseif ($available.Count -eq 1) {
        $branch = $available[0]
    } else {
        throw "Cannot safely choose a branch. Run again with -Branch <name>. Available: $($available -join ', ')."
    }

    # Security and correctness: validate branch syntax and do not force-push.
    Invoke-Checked 'git' @('check-ref-format', '--branch', $branch) | Out-Null
    Write-Host "Selected fork branch: $branch (origin/HEAD=$remoteHead)" -ForegroundColor Green
    Invoke-Checked 'git' @('-C', $CloneDir, 'checkout', '-B', $branch, "refs/remotes/origin/$branch") | Out-Null
    $checkedOut = @(& git -C $CloneDir symbolic-ref --short HEAD)
    if ($LASTEXITCODE -ne 0 -or $checkedOut.Count -eq 0 -or $checkedOut[0] -cne $branch) {
        throw "Failed to checkout fork branch '$branch'."
    }
    return $branch
}
function Set-LocalGitCommitIdentity([string]$CloneDir, [string]$GitHubUser) {
    # Configure only the temporary clone, leaving the user's global Git settings untouched.
    $authorName = (& git -C $CloneDir config user.name | Select-Object -First 1)
    if ([string]::IsNullOrWhiteSpace($authorName)) {
        Invoke-Checked 'git' @('-C', $CloneDir, 'config', 'user.name', $GitHubUser)
    }
    $authorEmail = (& git -C $CloneDir config user.email | Select-Object -First 1)
    if ([string]::IsNullOrWhiteSpace($authorEmail)) {
        $userId = (& gh api user --jq '.id' | Select-Object -First 1)
        if ($LASTEXITCODE -eq 0 -and $userId -match '^\d+$') {
            $noreply = "${userId}+${GitHubUser}@users.noreply.github.com"
        } else {
            # Historical GitHub noreply format; Git may accept it even if it is
            # not linked to the account. Do not abort publication for this reason.
            $noreply = "${GitHubUser}@users.noreply.github.com"
            Write-Warning 'Could not obtain numeric GitHub user ID; using username-based noreply email locally.'
        }
        Invoke-Checked 'git' @('-C', $CloneDir, 'config', 'user.email', $noreply)
    }
}

try {
    Write-Host '== Xbox remapper: GitHub fork + release publisher ==' -ForegroundColor Cyan
    Write-Host "Project: $ProjectRoot"
    Write-Host "Release: $ReleaseDir"
    Write-Host "Tag:     $Tag"

    foreach ($fileName in @('main.cpp', 'XboxMultiClick.h', 'TaskViewDetector.h',
                           'TaskViewXGuard.h', 'Xbox Controller button remapper.sln')) {
        Assert-File (Join-Path $ProjectRoot $fileName)
    }
    Assert-File (Join-Path $ReleaseDir 'SDL3.dll')
    Assert-File (Join-Path $ReleaseDir 'config.ini')

    $exeCandidates = @(Get-ChildItem -LiteralPath $ReleaseDir -Filter '*.exe' -File)
    $preferredExe = @($exeCandidates | Where-Object { $_.Name -ieq 'Xbox Controller Button Remapper.exe' })
    if ($preferredExe.Count -eq 1) {
        $exe = $preferredExe[0]
    } elseif ($exeCandidates.Count -eq 1) {
        $exe = $exeCandidates[0]
    } else {
        throw "Expected exactly one EXE in $ReleaseDir (found: $($exeCandidates.Name -join ', '))."
    }
    Assert-File $exe.FullName

    Write-Host ''
    Write-Host 'The following three files will be PUBLIC GitHub Release assets:' -ForegroundColor Yellow
    foreach ($path in @((Join-Path $ReleaseDir 'SDL3.dll'),
                        (Join-Path $ReleaseDir 'config.ini'), $exe.FullName)) {
        $item = Get-Item -LiteralPath $path
        Write-Host ('  {0} ({1:N0} bytes)' -f $item.Name, $item.Length)
    }
    Write-Host 'Personal config.ini is uploaded AS-IS. It is not redacted or regenerated.' -ForegroundColor Yellow
    Write-Host 'Source code will be committed to your own fork, not the upstream repository.'
    if (-not $Yes) {
        $confirmation = Read-Host 'Publish this source + PERSONAL config publicly? Type YES to continue'
        if ($confirmation -cne 'YES') {
            Write-Host 'Canceled. No GitHub changes made.' -ForegroundColor Yellow
            return
        }
    }

    Assert-Command 'git' 'Install Git for Windows: winget install --id Git.Git -e'
    Assert-Command 'gh'  'Install GitHub CLI: winget install --id GitHub.cli -e'

    New-Item -ItemType Directory -Path $TempRoot -Force | Out-Null
    Push-Location $TempRoot
    try {
        if (-not (Test-CommandSuccess 'gh' @('auth','status','--hostname','github.com'))) {
            Write-Host 'GitHub CLI is not logged in. Opening browser login...' -ForegroundColor Yellow
            Invoke-Checked 'gh' @('auth','login','--hostname','github.com','--web')
        }
        # gh can configure Git's credential helper for HTTPS git push.
        Invoke-Checked 'gh' @('auth','setup-git')
        $GitHubUser = (& gh api user --jq '.login' | Select-Object -First 1)
        if ($LASTEXITCODE -ne 0 -or [string]::IsNullOrWhiteSpace($GitHubUser)) {
            throw 'Unable to determine authenticated GitHub username.'
        }
        $GitHubUser = $GitHubUser.Trim()
        $ForkRepo = "$GitHubUser/$RepoName"
        Write-Host "GitHub account: $GitHubUser" -ForegroundColor Green

        $forkExists = Test-CommandSuccess 'gh' @('repo','view',$ForkRepo,'--json','nameWithOwner','--jq','.nameWithOwner')
        if (-not $forkExists) {
            Write-Host "Forking $UpstreamRepo ..." -ForegroundColor Cyan
            Invoke-Checked 'gh' @('repo', 'fork', $UpstreamRepo, '--clone=false')
        } else {
            Write-Host "Using existing repository: $ForkRepo"
        }
        $parentName = (& gh api "repos/$ForkRepo" --jq '.parent.full_name' | Select-Object -First 1)
        if ($LASTEXITCODE -ne 0 -or $parentName -ne $UpstreamRepo) {
            throw "Repository $ForkRepo is not a fork of $UpstreamRepo. Refusing to overwrite it."
        }

        if (Test-CommandSuccess 'gh' @('release','view',$Tag,'--repo',$ForkRepo,'--json','tagName')) {
            throw "Release tag '$Tag' already exists in $ForkRepo. Re-run with -Tag v2.0.0-custom.2 (or another new tag)."
        }
        $cloneDir = Join-Path $TempRoot 'fork'
        Write-Host 'Cloning the fork to a temporary directory...' -ForegroundColor Cyan
        Invoke-Checked 'gh' @('repo', 'clone', $ForkRepo, $cloneDir, '--no-upstream')
        $branch = Resolve-ForkBranch $ForkRepo $cloneDir $Branch
        Set-LocalGitCommitIdentity $cloneDir $GitHubUser
        if (Test-CommandSuccess 'git' @('-C',$cloneDir,'ls-remote','--exit-code','--tags','origin',"refs/tags/$Tag")) {
            throw "Git tag '$Tag' already exists in the fork. Choose a new tag."
        }

        Write-Host 'Copying local FINAL source (excluding x64 binaries and build folders)...' -ForegroundColor Cyan
        Copy-SourceTree $ProjectRoot $cloneDir
        Invoke-Checked 'git' @('-C',$cloneDir,'add','-A')
        Invoke-Checked 'git' @('-C',$cloneDir,'status','--short')

        $assetDir = Join-Path $TempRoot 'assets'
        New-Item -ItemType Directory -Path $assetDir -Force | Out-Null
        $dllDst = Join-Path $assetDir 'SDL3.dll'
        $iniDst = Join-Path $assetDir 'config.ini'
        $exeDst = Join-Path $assetDir $exe.Name
        Copy-Verified (Join-Path $ReleaseDir 'SDL3.dll') $dllDst
        Copy-Verified (Join-Path $ReleaseDir 'config.ini') $iniDst
        Copy-Verified $exe.FullName $exeDst
        $zipPath = Join-Path $TempRoot ("Xbox-Remapper-Windows-x64-$Tag.zip")
        Compress-Archive -LiteralPath @($dllDst, $iniDst, $exeDst) -DestinationPath $zipPath -CompressionLevel Optimal -Force
        if (-not (Test-Path -LiteralPath $zipPath)) { throw 'ZIP creation failed.' }

        $changes = (& git -C $cloneDir status --porcelain)
        if ($LASTEXITCODE -ne 0) { throw 'Could not inspect Git changes.' }
        if ($changes) {
            Write-Host 'Committing final source changes...' -ForegroundColor Cyan
            Invoke-Checked 'git' @('-C',$cloneDir,'commit','-m','Add configurable Xbox multi-click and Task View X/Del enhancements')
            Write-Host 'Pushing source to fork...' -ForegroundColor Cyan
            Invoke-Checked 'git' @('-C',$cloneDir,'push','origin',"HEAD:$branch")
        } else {
            Write-Host 'Source already matches fork; no new commit is needed.'
        }

        $notes = @'
Customized fork of Adam777Z/xbox-controller-button-remapper.

- Xbox button multi-click actions (single/double/triple), configurable in config.ini
- Joystick raw button input support for pulse-style controller buttons
- Native screenshot / Game Bar / Windows Task View shortcuts
- Optional Task View-only X -> additional Delete action
- Windows controller Game Bar shortcut control and debug improvements

Windows x64 files are attached separately, plus a portable ZIP containing exactly:
- SDL3.dll
- config.ini (personal configuration copied exactly from local x64\Release)
- Xbox Controller Button Remapper.exe (or the locally built EXE filename)

This Release is a customized build, not an official upstream release.
The config.ini file is the publisher's PERSONAL configuration and is publicly visible.
'@
        $notesPath = Join-Path $TempRoot 'RELEASE_NOTES.txt'
        [System.IO.File]::WriteAllText($notesPath, $notes, (New-Object System.Text.UTF8Encoding($false)))
        Write-Host 'Publishing GitHub Release assets...' -ForegroundColor Cyan
        $title = "Xbox Remapper - Custom $Tag"
        Invoke-Checked 'gh' @('release','create',$Tag,$dllDst,$iniDst,$exeDst,$zipPath,
                              '--repo',$ForkRepo,'--title',$title,
                              '--notes-file',$notesPath,'--target',$branch)
        $Succeeded = $true
        Write-Host ''
        Write-Host "SOURCE:  https://github.com/$ForkRepo" -ForegroundColor Green
        Write-Host "RELEASE: https://github.com/$ForkRepo/releases/tag/$Tag" -ForegroundColor Green
        Write-Host 'Done. Published three individual files and one portable ZIP.' -ForegroundColor Green
    } finally {
        Pop-Location
    }
} catch {
    Write-Host ''
    Write-Host ('FAILED: ' + $_.Exception.Message) -ForegroundColor Red
    if ($_.InvocationInfo -and $_.InvocationInfo.ScriptLineNumber) {
        Write-Host ('At script line: ' + $_.InvocationInfo.ScriptLineNumber) -ForegroundColor Yellow
    }
    if (Test-Path -LiteralPath $TempRoot) {
        Write-Host "Temporary files kept for inspection: $TempRoot" -ForegroundColor Yellow
    }
    exit 1
} finally {
    if ($Succeeded -and (Test-Path -LiteralPath $TempRoot)) {
        try { Remove-Item -LiteralPath $TempRoot -Recurse -Force -ErrorAction Stop }
        catch { Write-Host "Note: unable to remove temporary folder $TempRoot" -ForegroundColor Yellow }
    }
}

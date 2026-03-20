function Invoke-VersionAndDllChecks {
    param(
        [string]$Path,
        [string]$ScriptVersion,
        [string]$Repo
    )
    try {
        Write-Verbose "Checking for new releases on GitHub..."
        $latestRelease = Invoke-RestMethod -Uri "https://api.github.com/repos/$Repo/releases/latest" -ErrorAction Stop
        $latestVersion = $latestRelease.tag_name.TrimStart('v')

        if ([version]$latestVersion -gt [version]$ScriptVersion) {
            Write-Warning "A new version of this script is available: v$latestVersion"
            Write-Warning "Download it here: $($latestRelease.html_url)"
        }

        $dllsToVerify = @("dbgcore.dll", "pnsradgameserver.dll")
        $echoBinDir = Split-Path -Path $Path -Parent
        foreach ($dllName in $dllsToVerify) {
            $localDllPath = Join-Path $echoBinDir $dllName
            $releaseAsset = $latestRelease.assets | Where-Object { $_.name -eq $dllName }

            if (-not $releaseAsset) {
                Write-Verbose "Could not find '$dllName' in the latest release assets."
                continue
            }

            if (-not (Test-Path $localDllPath)) {
                Write-Warning "Required file '$dllName' is missing from '$echoBinDir'."
                Write-Warning "Download it from: $($latestRelease.html_url)"
                continue
            }

            $localHash = (Get-FileHash $localDllPath).Hash
            $remoteDllBytes = Invoke-WebRequest -Uri $releaseAsset.browser_download_url -UseBasicParsing
            $remoteHash = (Get-FileHash -InputStream $remoteDllBytes.ContentStream).Hash

            if ($localHash -ne $remoteHash) {
                Write-Warning "Your version of '$dllName' is outdated."
                Write-Warning "Download the latest version from: $($latestRelease.html_url)"
            } else {
                Write-Verbose "'$dllName' is up to date."
            }
        }
    } catch {
        Write-Warning "Could not check for updates. Please check your internet connection."
    }
}

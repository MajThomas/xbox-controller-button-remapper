# Disable only the Windows Xbox controller GUIDE->Game Bar shortcut.
# The original value (including an absent value) is backed up once.
# Win+G, capture APIs, and the Game Bar app are NOT disabled.
$ErrorActionPreference = 'Stop'
$target = 'HKCU:\Software\Microsoft\GameBar'
$backup = 'HKCU:\Software\XboxControllerButtonRemapper\GameBarOverride'
$name = 'UseNexusForGameBarEnabled'

try {
    New-Item -Path $target -Force | Out-Null
    $current = Get-ItemProperty -Path $target -Name $name -ErrorAction SilentlyContinue
    if ($null -ne $current -and $null -ne $current.$name -and [int]$current.$name -eq 0) {
        Write-Host 'Already disabled. No settings changed.'
        exit 0
    }

    New-Item -Path $backup -Force | Out-Null
    $saved = Get-ItemProperty -Path $backup -Name OriginalStateSaved -ErrorAction SilentlyContinue
    if ($null -eq $saved -or [int]$saved.OriginalStateSaved -ne 1) {
        $exists = if ($null -ne $current -and $null -ne $current.$name) { 1 } else { 0 }
        $oldValue = if ($exists) { [int]$current.$name } else { 0 }
        New-ItemProperty -Path $backup -Name OriginalExists -Value $exists -PropertyType DWord -Force | Out-Null
        New-ItemProperty -Path $backup -Name OriginalValue -Value $oldValue -PropertyType DWord -Force | Out-Null
        New-ItemProperty -Path $backup -Name OriginalStateSaved -Value 1 -PropertyType DWord -Force | Out-Null
    }

    New-ItemProperty -Path $target -Name $name -Value 0 -PropertyType DWord -Force | Out-Null
    Write-Host 'Controller Xbox button no longer launches Game Bar through the Windows shortcut.'
    Write-Host 'Keyboard Win+G is unchanged. Sign out / restart Windows if needed.'
} catch {
    Write-Error "Could not update controller Game Bar shortcut: $_"
    exit 1
}

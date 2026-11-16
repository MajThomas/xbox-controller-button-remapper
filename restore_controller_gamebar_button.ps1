# Restore the exact original per-user Windows controller GUIDE/Game Bar setting.
# Before use, set [XboxMultiClick] disable_system_gamebar_button=0 and exit the app,
# or the remapper will disable the controller shortcut again at next launch.
$ErrorActionPreference = 'Stop'
$target = 'HKCU:\Software\Microsoft\GameBar'
$backup = 'HKCU:\Software\XboxControllerButtonRemapper\GameBarOverride'
$name = 'UseNexusForGameBarEnabled'

try {
    if (-not (Test-Path $backup)) {
        Write-Warning 'No saved original setting was found. Nothing was changed.'
        Write-Host 'You can still enable the controller shortcut manually under Settings > Gaming > Game Bar.'
        exit 2
    }
    $original = Get-ItemProperty -Path $backup
    if ([int]$original.OriginalStateSaved -ne 1) {
        throw 'The saved setting is incomplete. Refusing to guess the original value.'
    }
    if ([int]$original.OriginalExists -eq 1) {
        New-Item -Path $target -Force | Out-Null
        New-ItemProperty -Path $target -Name $name -Value ([int]$original.OriginalValue) -PropertyType DWord -Force | Out-Null
    } elseif ([int]$original.OriginalExists -eq 0) {
        if (Test-Path $target) {
            Remove-ItemProperty -Path $target -Name $name -ErrorAction SilentlyContinue
        }
    } else {
        throw 'The saved setting contains an invalid original-exists flag.'
    }
    Remove-Item -Path $backup -Force
    Write-Host 'The original Windows Xbox controller Game Bar setting was restored.'
    Write-Host 'Sign out / restart Windows if needed.'
} catch {
    Write-Error "Could not restore controller Game Bar shortcut: $_"
    exit 1
}

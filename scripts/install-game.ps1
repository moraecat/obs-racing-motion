[CmdletBinding()]
param([Parameter(Mandatory)][ValidateSet('ETS2','ATS')][string]$Game)
$ErrorActionPreference = 'Stop'
try {
    Add-Type -AssemblyName System.Windows.Forms
    $dialog = New-Object System.Windows.Forms.FolderBrowserDialog
    $dialog.Description = "Select the $Game game root folder (contains bin/win_x64). Steam: Properties > Installed Files > Browse."
    $dialog.ShowNewFolderButton = $false
    try {
        if ($dialog.ShowDialog() -ne [System.Windows.Forms.DialogResult]::OK) {
            Write-Output 'Installation cancelled. No files copied.'
            exit 2
        }
        $selected = $dialog.SelectedPath
    } finally { $dialog.Dispose() }
    $arguments = @{}
    if ($Game -eq 'ETS2') { $arguments['ETS2Path'] = $selected }
    else { $arguments['ATSPath'] = $selected }
    & (Join-Path $PSScriptRoot 'install.ps1') @arguments
    exit 0
} catch {
    Write-Error $_
    exit 1
}

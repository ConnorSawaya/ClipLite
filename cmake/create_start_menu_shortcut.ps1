param(
    [Parameter(Mandatory = $true)] [string] $ShortcutPath,
    [Parameter(Mandatory = $true)] [string] $TargetPath,
    [Parameter(Mandatory = $true)] [string] $WorkingDirectory,
    [Parameter(Mandatory = $true)] [string] $IconPath
)

$shell = New-Object -ComObject WScript.Shell
$shortcut = $shell.CreateShortcut($ShortcutPath)
$shortcut.TargetPath = $TargetPath
$shortcut.Arguments = "--open"
$shortcut.WorkingDirectory = $WorkingDirectory
$shortcut.IconLocation = "$IconPath,0"
$shortcut.Description = "ClipLite - Instant replay clipper"
$shortcut.Save()

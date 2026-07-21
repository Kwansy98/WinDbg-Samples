[CmdletBinding()]
param(
    [string] $Destination,

    [string] $WindbgSkillRelease = (Join-Path (Split-Path $PSScriptRoot -Parent) 'windbgskill\windbgskill\x64\Release'),

    [string] $WindbgSkillLicense = (Join-Path (Split-Path $PSScriptRoot -Parent) 'windbgskill\LICENSE')
)

$ErrorActionPreference = 'Stop'
$repo = $PSScriptRoot
if ([string]::IsNullOrWhiteSpace($Destination)) {
    $Destination = Join-Path $repo 'artifacts\VMwareEXDI'
}
$destinationPath = [System.IO.Path]::GetFullPath($Destination)

if (Test-Path -LiteralPath $destinationPath) {
    if (-not (Test-Path -LiteralPath $destinationPath -PathType Container)) {
        throw "Destination is not a directory: $destinationPath"
    }
} else {
    New-Item -ItemType Directory -Path $destinationPath | Out-Null
}

$files = [ordered]@{
    'ExdiGdbSrv.dll' = Join-Path $repo 'Exdi\exdigdbsrv\Release\x64\ExdiGdbSrv.dll'
    'ExdiGdbSrv.pdb' = Join-Path $repo 'Exdi\exdigdbsrv\Release\x64\ExdiGdbSrv.pdb'
    'windbgskill.dll' = Join-Path $WindbgSkillRelease 'windbgskill.dll'
    'windbgskill.pdb' = Join-Path $WindbgSkillRelease 'windbgskill.pdb'
    'exdiConfigData.xml' = Join-Path $repo 'Exdi\exdigdbsrv\GdbSrvControllerLib\exdiConfigData.xml'
    'systemregisters.xml' = Join-Path $repo 'Exdi\exdigdbsrv\GdbSrvControllerLib\systemregisters.xml'
    'exdi_manager.py' = Join-Path $repo 'exdi_manager.py'
    'exdi_manager.bat' = Join-Path $repo 'exdi_manager.bat'
    'README.md' = Join-Path $repo 'docs\exdi-manager.md'
    'LICENSE-WinDbg-Samples.txt' = Join-Path $repo 'LICENSE'
    'Third-Party-Notices.txt' = Join-Path $repo 'Third Party Notices.txt'
    'LICENSE-windbgskill.txt' = $WindbgSkillLicense
}

foreach ($source in $files.Values) {
    if (-not (Test-Path -LiteralPath $source -PathType Leaf)) {
        throw "Required package input does not exist: $source"
    }
}

foreach ($entry in $files.GetEnumerator()) {
    Copy-Item -LiteralPath $entry.Value -Destination (Join-Path $destinationPath $entry.Key) -Force
}

[pscustomobject]@{
    Destination = $destinationPath
    Files = $files.Count
    PersistentConfigPreserved = Test-Path -LiteralPath (Join-Path $destinationPath 'exdi_manager.json')
    RuntimeLogPreserved = Test-Path -LiteralPath (Join-Path $destinationPath 'exdi_manager.log')
}

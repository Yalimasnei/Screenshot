# SPDX-License-Identifier: GPL-3.0-only
# Export only reviewed source inputs; verify the archive and unchanged originals.
param([string]$OutputZip)
$ErrorActionPreference = 'Stop'
$taskRoot = [IO.Path]::GetFullPath((Split-Path $PSScriptRoot))
$taskRootFiles = @('.gitignore', 'LICENSE', 'README.md')
$taskRelative = [Collections.Generic.List[string]]::new()
foreach ($taskName in $taskRootFiles) { $taskRelative.Add($taskName) }
foreach ($taskName in @('src', 'scripts', 'resources', 'docs')) {
    $taskDirectory = Get-Item -LiteralPath (Join-Path $taskRoot $taskName)
    if ($taskDirectory.Attributes -band [IO.FileAttributes]::ReparsePoint) { throw "Linked directory is not allowed: $taskName" }
    $taskPending = [Collections.Generic.Stack[IO.DirectoryInfo]]::new()
    $taskPending.Push($taskDirectory)
    while ($taskPending.Count) {
        $taskCurrent = $taskPending.Pop()
        foreach ($taskItem in Get-ChildItem -LiteralPath $taskCurrent.FullName -Force) {
            if ($taskItem.Attributes -band [IO.FileAttributes]::ReparsePoint) { throw 'Linked source entries are not allowed' }
            if ($taskItem.PSIsContainer) { $taskPending.Push($taskItem); continue }
            $taskAllowed = switch ($taskName) {
                'src' { $taskItem.Extension -in @('.cpp', '.h') }
                'scripts' { $taskItem.Extension -eq '.ps1' }
                'resources' { $taskItem.Extension -in @('.rc', '.manifest') }
                'docs' { $taskItem.Extension -eq '.md' }
            }
            if ($taskAllowed) { $taskRelative.Add([IO.Path]::GetRelativePath($taskRoot, $taskItem.FullName).Replace('\', '/')) }
        }
    }
}
$taskVendor = Get-Item -LiteralPath (Join-Path $taskRoot 'third_party/nv-codec-headers')
if ($taskVendor.Attributes -band [IO.FileAttributes]::ReparsePoint) { throw 'Linked third_party directory is not allowed' }
$taskVendorRoot = Get-Item -LiteralPath (Join-Path $taskRoot 'third_party')
if ($taskVendorRoot.Attributes -band [IO.FileAttributes]::ReparsePoint) { throw 'Linked third_party directory is not allowed' }
$taskRelative.Add('third_party/nv-codec-headers/nvEncodeAPI.h')
$taskRelative = @($taskRelative | Sort-Object -Unique)
$taskManifest = foreach ($taskName in $taskRelative) {
    $taskFile = Get-Item -LiteralPath (Join-Path $taskRoot $taskName)
    if ($taskFile.PSIsContainer -or ($taskFile.Attributes -band [IO.FileAttributes]::ReparsePoint)) {
        throw "Only ordinary files may be exported: $taskName"
    }
    [ordered]@{ path = $taskName; bytes = $taskFile.Length; sha256 = (Get-FileHash -LiteralPath $taskFile.FullName -Algorithm SHA256).Hash }
}
if (-not $OutputZip) { $OutputZip = Join-Path $taskRoot 'build/packages/Screenshot-source.zip' }
$taskOutput = [IO.Path]::GetFullPath($OutputZip)
if (Test-Path -LiteralPath $taskOutput) { throw "Refusing to overwrite an existing archive: $taskOutput" }
$taskParent = Split-Path $taskOutput
New-Item -ItemType Directory -Path $taskParent -Force | Out-Null
$taskStream = [IO.File]::Open($taskOutput, [IO.FileMode]::CreateNew, [IO.FileAccess]::ReadWrite, [IO.FileShare]::None)
$taskZip = [IO.Compression.ZipArchive]::new($taskStream, [IO.Compression.ZipArchiveMode]::Create, $false)
try {
    foreach ($taskItem in $taskManifest) {
        [IO.Compression.ZipFileExtensions]::CreateEntryFromFile($taskZip, (Join-Path $taskRoot $taskItem.path),
            ('Screenshot/' + $taskItem.path), [IO.Compression.CompressionLevel]::Optimal) | Out-Null
    }
    $taskEntry = $taskZip.CreateEntry('Screenshot/SOURCE-MANIFEST.json')
    $taskWriter = [IO.StreamWriter]::new($taskEntry.Open(), [Text.UTF8Encoding]::new($false))
    try { $taskWriter.Write((ConvertTo-Json -InputObject @($taskManifest) -Depth 4)) } finally { $taskWriter.Dispose() }
} finally { $taskZip.Dispose() }
$taskZip = [IO.Compression.ZipFile]::OpenRead($taskOutput)
try {
    if ($taskZip.Entries.Count -ne $taskManifest.Count + 1) { throw 'Unexpected archive entries' }
    foreach ($taskItem in $taskManifest) {
        $taskEntry = $taskZip.GetEntry('Screenshot/' + $taskItem.path)
        if (-not $taskEntry -or $taskEntry.Length -ne $taskItem.bytes) { throw "Missing or changed archive entry: $($taskItem.path)" }
        $taskInput = $taskEntry.Open()
        $taskHash = [Security.Cryptography.SHA256]::Create()
        try { $taskDigest = [Convert]::ToHexString($taskHash.ComputeHash($taskInput)) }
        finally { $taskInput.Dispose(); $taskHash.Dispose() }
        if ($taskDigest -ne $taskItem.sha256 -or (Get-FileHash -LiteralPath (Join-Path $taskRoot $taskItem.path) -Algorithm SHA256).Hash -ne $taskItem.sha256) {
            throw "Source or archive changed during export: $($taskItem.path)"
        }
    }
} finally { $taskZip.Dispose() }
[pscustomobject]@{ Archive = $taskOutput; Files = $taskManifest.Count; Verified = $true; SHA256 = (Get-FileHash -LiteralPath $taskOutput -Algorithm SHA256).Hash }

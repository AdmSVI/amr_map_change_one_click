param(
    [string]$QtRoot = "",

    [string]$VCRedistPath = "",

    [string]$Version = "0.3.0",
    [string]$InnoSetupPath = "C:\Program Files (x86)\Inno Setup 6\ISCC.exe"
)

$ErrorActionPreference = "Stop"
$projectDir = [System.IO.Path]::GetFullPath((Join-Path $PSScriptRoot ".."))
$buildDir = Join-Path $projectDir "build\package-windows"
$stageDir = Join-Path $projectDir "dist\windows\stage"
$outputDir = Join-Path $projectDir "dist\windows"

if (-not $QtRoot) {
    $QtRoot = Get-ChildItem -Path "C:\Qt\*\msvc2022_64" -Directory -ErrorAction SilentlyContinue |
        Where-Object { Test-Path -LiteralPath (Join-Path $_.FullName "bin\windeployqt.exe") } |
        Sort-Object FullName -Descending |
        Select-Object -First 1 -ExpandProperty FullName
}

if (-not $VCRedistPath) {
    $vswhere = "${env:ProgramFiles(x86)}\Microsoft Visual Studio\Installer\vswhere.exe"
    if (Test-Path -LiteralPath $vswhere) {
        $vsRoot = & $vswhere -latest -products * -property installationPath
        if ($vsRoot) {
            $VCRedistPath = Get-ChildItem -Path (Join-Path $vsRoot "VC\Redist\MSVC") `
                -Recurse -Filter vc_redist.x64.exe -File -ErrorAction SilentlyContinue |
                Sort-Object LastWriteTime -Descending |
                Select-Object -First 1 -ExpandProperty FullName
        }
    }
}

if (-not (Get-Command cmake -ErrorAction SilentlyContinue)) {
    throw "CMake was not found. Install CMake and reopen PowerShell before packaging."
}

if (-not $QtRoot) {
    throw "Qt MSVC 2022 64-bit was not found. Install it under C:\Qt, or pass -QtRoot with its actual directory."
}

if (-not $VCRedistPath) {
    throw "VC_redist.x64.exe was not found. Install the Visual Studio 2022 C++ workload, or pass -VCRedistPath with the downloaded file."
}

$qtDeploy = Join-Path $QtRoot "bin\windeployqt.exe"
$appExe = Join-Path $buildDir "Release\amr-map-manager.exe"

foreach ($requiredFile in @($qtDeploy, $VCRedistPath, $InnoSetupPath)) {
    if (-not (Test-Path -LiteralPath $requiredFile -PathType Leaf)) {
        throw "Required file not found: $requiredFile"
    }
}

cmake -S $projectDir -B $buildDir -G "Visual Studio 17 2022" -A x64 `
    "-DCMAKE_PREFIX_PATH=$QtRoot"
if ($LASTEXITCODE -ne 0) { throw "CMake configuration failed with exit code $LASTEXITCODE" }
cmake --build $buildDir --config Release --parallel
if ($LASTEXITCODE -ne 0) { throw "CMake build failed with exit code $LASTEXITCODE" }

if (-not (Test-Path -LiteralPath $appExe -PathType Leaf)) {
    throw "Release executable was not created: $appExe"
}

$resolvedOutput = [System.IO.Path]::GetFullPath($outputDir)
$resolvedStage = [System.IO.Path]::GetFullPath($stageDir)
if (-not $resolvedStage.StartsWith($resolvedOutput, [System.StringComparison]::OrdinalIgnoreCase)) {
    throw "Unsafe staging path: $resolvedStage"
}
if (Test-Path -LiteralPath $resolvedStage) {
    Remove-Item -LiteralPath $resolvedStage -Recurse -Force
}
New-Item -ItemType Directory -Force -Path $resolvedStage | Out-Null

Copy-Item -LiteralPath $appExe -Destination $resolvedStage
$agentExe = Join-Path $buildDir "Release\amr-map-agent.exe"
Copy-Item -LiteralPath $agentExe -Destination $resolvedStage
Copy-Item -LiteralPath (Join-Path $projectDir "agent.ini.example") -Destination $resolvedStage
& $qtDeploy --release --no-translations --no-compiler-runtime --dir $resolvedStage $appExe
if ($LASTEXITCODE -ne 0) { throw "windeployqt failed with exit code $LASTEXITCODE" }

& $qtDeploy --release --no-translations --no-compiler-runtime --dir $resolvedStage $agentExe
if ($LASTEXITCODE -ne 0) { throw "Agent windeployqt failed with exit code $LASTEXITCODE" }

$portablePath = Join-Path $resolvedOutput "AMRMapManager-$Version-win64-portable.zip"
if (Test-Path -LiteralPath $portablePath) { Remove-Item -LiteralPath $portablePath -Force }
Compress-Archive -Path (Join-Path $resolvedStage "*") -DestinationPath $portablePath -CompressionLevel Optimal

Copy-Item -LiteralPath $VCRedistPath -Destination (Join-Path $resolvedStage "VC_redist.x64.exe")
$issFile = Join-Path $projectDir "packaging\windows\amr-map-manager.iss"
$appIcon = Join-Path $projectDir "assets\icons\svi-logo.ico"
& $InnoSetupPath "/DAppVersion=$Version" "/DSourceDir=$resolvedStage" `
    "/DOutputDir=$resolvedOutput" "/DAppIcon=$appIcon" $issFile
if ($LASTEXITCODE -ne 0) { throw "Inno Setup failed with exit code $LASTEXITCODE" }

$installerPath = Join-Path $resolvedOutput "AMRMapManager-$Version-win64-setup.exe"
$hashes = Get-FileHash -Algorithm SHA256 $installerPath, $portablePath
$hashLines = $hashes | ForEach-Object { "$($_.Hash.ToLower())  $([System.IO.Path]::GetFileName($_.Path))" }
[System.IO.File]::WriteAllLines((Join-Path $resolvedOutput "SHA256SUMS.txt"), $hashLines)
$hashes | Format-Table -AutoSize

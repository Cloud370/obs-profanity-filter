$ErrorActionPreference = 'Stop'
$ProjectRoot = $PSScriptRoot

# Change to project root
Set-Location $ProjectRoot

# Check for Inno Setup
$ISCC = "C:\Program Files (x86)\Inno Setup 6\ISCC.exe"
if (-not (Test-Path $ISCC)) {
    $ISCC_CMD = Get-Command iscc -ErrorAction SilentlyContinue
    if ($ISCC_CMD) {
        $ISCC = $ISCC_CMD.Source
    } else {
        Write-Warning "Inno Setup Compiler (ISCC.exe) not found in default location."
        Write-Warning "Please ensure Inno Setup is installed and ISCC is in your PATH."
    }
}

Write-Host "Found ISCC: $ISCC"

# Configure
Write-Host "Configuring CMake..."
cmake -S . -B build -A x64

# Build
Write-Host "Building Release..."
cmake --build build --config Release --parallel

# Install to Package
Write-Host "Creating Package Structure..."
$PackageDir = "$ProjectRoot/release/Package"
if (Test-Path $PackageDir) { Remove-Item $PackageDir -Recurse -Force }
cmake --install build --prefix "$ProjectRoot/release/Package" --config Release

# Run ISCC
Write-Host "Compiling Installer..."
$IssFile = "$ProjectRoot/build/installer.iss"

if (Test-Path $ISCC) {
    & $ISCC $IssFile
} else {
    iscc $IssFile
}

Write-Host "Done! Check release folder or build folder for the output."

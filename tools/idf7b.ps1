# idf.py for the Waveshare 7B build: its own build dir and sdkconfig fragments.
# Needs ESP-IDF 5.5 activated in this PowerShell window (export.ps1).
# Runs from any directory. Examples:
#   .\tools\idf7b.ps1 build
#   .\tools\idf7b.ps1 -p COM5 app-flash monitor

if (-not $env:IDF_PYTHON_ENV_PATH -or $env:IDF_PATH -notmatch '5\.5') {
    Write-Error "ESP-IDF 5.5 isn't activated in this window (IDF_PATH=$env:IDF_PATH). Run its export.ps1 first."
    exit 1
}

Push-Location (Split-Path $PSScriptRoot -Parent)
try {
    # the venv's python directly: export.ps1 doesn't define idf.py globally
    & "$env:IDF_PYTHON_ENV_PATH\Scripts\python.exe" "$env:IDF_PATH\tools\idf.py" `
        -B build-7b `
        -D "SDKCONFIG_DEFAULTS=sdkconfig.defaults;sdkconfig.board.7b" `
        -D SDKCONFIG=build-7b/sdkconfig `
        @args
    $code = $LASTEXITCODE
} finally {
    Pop-Location
}
exit $code

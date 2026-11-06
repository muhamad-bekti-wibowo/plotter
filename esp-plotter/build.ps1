# Build firmware:  .\build.ps1 [argumen idf.py, mis. "-p COM5 flash monitor"]
# Memakai python venv ESP-IDF 5.5 langsung supaya tidak bentrok dengan python sistem.
$ErrorActionPreference = 'Continue'
$idf = 'C:\Espressif\frameworks\esp-idf-v5.5.4'
$venv = 'C:\Espressif\python_env\idf5.5_py3.11_env\Scripts'
$env:IDF_PATH = $idf
$env:IDF_PYTHON_ENV_PATH = Split-Path $venv
$env:PATH = "$venv;" + $env:PATH
& "$venv\python.exe" "$idf\tools\activate.py" --export 2>$null | Out-Null
$exp = & "$venv\python.exe" "$idf\tools\idf_tools.py" export --format key-value 2>$null
foreach ($l in $exp) { if ($l -match '^(\w+)=(.*)$') { $n=$Matches[1]; $v=$Matches[2].Replace('%PATH%',$env:PATH).Replace('$PATH',$env:PATH); Set-Item "env:$n" $v } }
Set-Location $PSScriptRoot
if (-not (Test-Path sdkconfig)) { & "$venv\python.exe" "$idf\tools\idf.py" set-target esp32 }
if ($args.Count) { & "$venv\python.exe" "$idf\tools\idf.py" @($args -split ' ') } else { & "$venv\python.exe" "$idf\tools\idf.py" build }

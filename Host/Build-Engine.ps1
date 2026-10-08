param([ValidateSet('Debug','Release')][string]$Configuration='Release')
$ErrorActionPreference='Stop'
$engineBuild=Join-Path $PSScriptRoot 'enginebuild'
& cmake -S $PSScriptRoot -B $engineBuild -G 'Visual Studio 17 2022' -A x64
if($LASTEXITCODE -ne 0){throw 'PixSpectra engine configuration failed'}
& cmake --build $engineBuild --config $Configuration --parallel 4
if($LASTEXITCODE -ne 0){throw 'PixSpectra engine build failed'}

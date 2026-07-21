param(
    [ValidateSet('multi_core', 'multi_core_backpressure')]
    [string]$Case = 'multi_core',
    [string]$Config = (Join-Path $PSScriptRoot 'config\tm_ring_demo.toml'),
    [Alias('PemConfig')]
    [string]$DdrConfig = '',
    [string]$Output = (Join-Path (Get-Location) 'tm_ring_multi_core_result.txt'),
    [string]$Binary = $env:TM_RING_ESL_BINARY,
    [string[]]$Set = @()
)

$ErrorActionPreference = 'Stop'

if ([string]::IsNullOrWhiteSpace($Binary)) {
    $Binary = Join-Path $PSScriptRoot 'bin\tm_ring_esl.exe'
}
$resolvedBinary = (Resolve-Path -LiteralPath $Binary -ErrorAction SilentlyContinue)
if ($null -eq $resolvedBinary) {
    throw "Standalone runner not found: $Binary. Build tm_ring_esl_main.cc with the project ESL runtime, or set TM_RING_ESL_BINARY."
}
$resolvedConfig = (Resolve-Path -LiteralPath $Config -ErrorAction Stop).Path

$arguments = @('--config', $resolvedConfig, '--case', $Case, '--output', $Output)
if (-not [string]::IsNullOrWhiteSpace($DdrConfig)) {
    $arguments += @('--ddr-config', (Resolve-Path -LiteralPath $DdrConfig -ErrorAction Stop).Path)
}
foreach ($override in $Set) {
    $arguments += @('--set', $override)
}

& $resolvedBinary.Path @arguments
exit $LASTEXITCODE

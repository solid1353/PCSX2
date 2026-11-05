[CmdletBinding()]
param(
    [Parameter(Mandatory = $true)][string]$BenchmarkExecutable,
    [Parameter(Mandatory = $true)][string]$PngLibrary,
    [Parameter(Mandatory = $true)][string]$Image,
    [Parameter(Mandatory = $true)][string]$BaselineLibrary,
    [Parameter(Mandatory = $true)][string]$CandidateLibrary,
    [string]$TaskWorkRoot = $env:UN_TASK_WORK_ROOT,
    [ValidateRange(1, 20)][int]$Rounds = 6
)

$ErrorActionPreference = 'Stop'
if ([string]::IsNullOrWhiteSpace($TaskWorkRoot)) { throw 'UN_TASK_WORK_ROOT is required.' }
$taskRoot = [IO.Path]::GetFullPath($TaskWorkRoot)
$taskInputs = Join-Path $taskRoot 'inputs'
if (-not (Test-Path -LiteralPath $taskInputs -PathType Container)) { throw 'The task inputs directory must already exist.' }
$benchmark = (Resolve-Path -LiteralPath $BenchmarkExecutable).ProviderPath
$sourceFiles = @($PngLibrary, $Image, $BaselineLibrary, $CandidateLibrary)
$pngLibrary = Join-Path $taskInputs "png-benchmark-$PID-libpng16.dll"
$image = Join-Path $taskInputs "png-benchmark-$PID-input.png"
$baseline = Join-Path $taskInputs "png-benchmark-$PID-zlib.dll"
$candidate = Join-Path $taskInputs "png-benchmark-$PID-zlib-ng.dll"
$active = Join-Path $taskInputs 'z.dll'
$results = Join-Path $taskRoot "png-benchmark-$PID-results.jsonl"
$copiedFiles = @($pngLibrary, $image, $baseline, $candidate)
foreach ($path in ($copiedFiles + @($active, $results))) {
    if (Test-Path -LiteralPath $path) { throw "Benchmark destination already exists: $path" }
}
foreach ($path in $sourceFiles) {
    if (-not (Test-Path -LiteralPath $path -PathType Leaf)) { throw "Benchmark source is missing: $path" }
}

$createdFiles = [Collections.Generic.List[string]]::new()
try {
    for ($index = 0; $index -lt $sourceFiles.Count; $index++) {
        Copy-Item -LiteralPath $sourceFiles[$index] -Destination $copiedFiles[$index]
        $createdFiles.Add($copiedFiles[$index])
    }
    $createdFiles.Add($active)
    for ($round = 1; $round -le $Rounds; $round++) {
        $backends = if ($round % 2) { @('zlib', 'zlib-ng') } else { @('zlib-ng', 'zlib') }
        foreach ($size in @(@{Width=1000; Height=750; Repetitions=5}, @{Width=0; Height=0; Repetitions=2})) {
            foreach ($backend in $backends) {
                $backendPath = if ($backend -eq 'zlib') { $baseline } else { $candidate }
                Copy-Item -LiteralPath $backendPath -Destination $active -Force
                Write-Host "Round $round, $backend, requested $($size.Width) x $($size.Height): running"
                $json = & $benchmark $pngLibrary $image $size.Width $size.Height $size.Repetitions 9 $backend $round
                if ($LASTEXITCODE -ne 0) { throw "Benchmark failed for $backend in round $round." }
                $result = $json | ConvertFrom-Json
                if (-not $result.pixels_equal) { throw 'The encoded PNG did not preserve all pixels.' }
                [IO.File]::AppendAllText($results, $json + [Environment]::NewLine, [Text.UTF8Encoding]::new($false))
                $times = @($result.milliseconds | Sort-Object)
                $median = if ($times.Count % 2) { $times[[int][Math]::Floor($times.Count / 2)] } else {
                    ($times[$times.Count / 2 - 1] + $times[$times.Count / 2]) / 2
                }
                Write-Host ('Round {0}, {1}, {2} x {3}: {4:N2} ms, {5} bytes, identical pixels' -f
                    $round, $backend, $result.width, $result.height, $median, $result.png_bytes)
            }
        }
    }
    Write-Host "All $Rounds alternating rounds completed. Measurements: $results"
}
finally {
    foreach ($path in $createdFiles) {
        $resolved = [IO.Path]::GetFullPath($path)
        if (-not $resolved.StartsWith($taskInputs + '\', [StringComparison]::OrdinalIgnoreCase)) {
            throw 'Benchmark cleanup escaped its task inputs directory.'
        }
        if (Test-Path -LiteralPath $resolved -PathType Leaf) {
            Remove-Item -LiteralPath $resolved -ErrorAction Continue
        }
    }
}

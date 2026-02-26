# HRTF diagnostic: generate impulse responses via ffmpeg sofalizer for multiple SOFA files
# Run from any directory: powershell -ExecutionPolicy Bypass -File scripts\run_sofalizer_diagnostics.ps1

# cd to project root so all paths are relative — avoids drive-letter colon in ffmpeg filter strings
Set-Location "S:\code\mumble"

$saDir = "S:\code\steam-audio\core\data\hrtf"

# Copy Steam Audio SOFA files locally so ffmpeg can use relative (colon-free) paths
Write-Host "Copying Steam Audio SOFA files..." -ForegroundColor Cyan
foreach ($name in @("cipic_124", "sadie_d1", "sadie_h12")) {
    if (-not (Test-Path "$name.sofa")) {
        Copy-Item "$saDir\$name.sofa" ".\$name.sofa"
    }
}

$sofas = [ordered]@{
    "current"   = "data/hrtf/default.sofa"
    "cipic_124" = "cipic_124.sofa"
    "sadie_d1"  = "sadie_d1.sofa"
    "sadie_h12" = "sadie_h12.sofa"
}

# Step 1: generate Dirac impulse via Python (avoids PowerShell comma-splitting bug with lavfi)
Write-Host "Generating impulse via Python..." -ForegroundColor Cyan
uv run --with numpy --with scipy python -c "import numpy as np; import scipy.io.wavfile as w; d = np.zeros(4800, dtype=np.int16); d[0] = 32767; w.write('test_impulse.wav', 48000, d)"
if ($LASTEXITCODE -ne 0) { Write-Error "Failed to generate impulse"; exit 1 }

# Step 2: apply sofalizer at 0 / 90 / 180 / 270 degrees for each SOFA file
foreach ($entry in $sofas.GetEnumerator()) {
    $label = $entry.Key
    $sofa  = $entry.Value
    Write-Host ""
    Write-Host "=== $label ===" -ForegroundColor Yellow

    foreach ($deg in @(0, 90, 180, 270)) {
        $out = "hrtf_${label}_${deg}deg.wav"
        Write-Host "  rotation=$deg -> $out" -ForegroundColor Cyan
        & ffmpeg -i test_impulse.wav `
            -af "sofalizer=sofa=$sofa`:type=freq:rotation=$deg`:elevation=0" `
            -y $out
        if ($LASTEXITCODE -ne 0) { Write-Error "sofalizer failed ($label, $deg deg)"; exit 1 }
    }
}

# Step 3: spatialized voice clips (current SOFA only, for listening comparison)
Write-Host ""
Write-Host "=== voice clips (current SOFA) ===" -ForegroundColor Yellow
foreach ($deg in @(90, 180)) {
    $out = "voice_spatialized_${deg}deg.wav"
    Write-Host "  rotation=$deg -> $out" -ForegroundColor Cyan
    & ffmpeg -i mictest.mp3 `
        -af "sofalizer=sofa=data/hrtf/default.sofa:type=freq:rotation=$deg`:elevation=0" `
        -y $out
    if ($LASTEXITCODE -ne 0) { Write-Error "Voice sofalizer failed at $deg deg"; exit 1 }
}

Write-Host ""
Write-Host "Done. Files written to S:\code\mumble" -ForegroundColor Green
Write-Host "Now run: uv run --with numpy --with scipy --with matplotlib python scripts/analyze_hrtf.py"

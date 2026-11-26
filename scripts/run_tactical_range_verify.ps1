$ErrorActionPreference = 'Stop'
$repoRoot = Split-Path -Parent $PSScriptRoot
$cli = Join-Path $repoRoot 'build\Release\frame_warp_cli.exe'
$reportDir = Join-Path $repoRoot 'build\Release\captures\tactical_range'
New-Item -ItemType Directory -Force -Path $reportDir | Out-Null

Add-Type -AssemblyName System.Drawing
Add-Type -ReferencedAssemblies System.Drawing -TypeDefinition @"
using System;
using System.Drawing;
using System.Drawing.Imaging;
using System.IO;
using System.Runtime.InteropServices;

public static class RangeImgTools {
    public static void PpmToPng(string ppmPath, string pngPath) {
        byte[] raw = File.ReadAllBytes(ppmPath);
        int pos = 0;
        Func<string> nextToken = () => {
            while (pos < raw.Length && (raw[pos] <= 32 || raw[pos] == '#')) {
                if (raw[pos] == '#') { while (pos < raw.Length && raw[pos] != '\n') pos++; }
                else pos++;
            }
            int start = pos;
            while (pos < raw.Length && raw[pos] > 32) pos++;
            return System.Text.Encoding.ASCII.GetString(raw, start, pos - start);
        };
        if (nextToken() != "P6") throw new Exception("Expected P6");
        int w = int.Parse(nextToken());
        int h = int.Parse(nextToken());
        nextToken();
        if (pos < raw.Length && raw[pos] <= 32) pos++;
        using (Bitmap bmp = new Bitmap(w, h, PixelFormat.Format24bppRgb)) {
            BitmapData bd = bmp.LockBits(new Rectangle(0, 0, w, h), ImageLockMode.WriteOnly, PixelFormat.Format24bppRgb);
            byte[] bgr = new byte[bd.Stride * h];
            for (int y = 0; y < h; y++) {
                int s = pos + y * w * 3;
                int d = y * bd.Stride;
                for (int x = 0; x < w; x++) {
                    bgr[d + x * 3 + 0] = raw[s + x * 3 + 2];
                    bgr[d + x * 3 + 1] = raw[s + x * 3 + 1];
                    bgr[d + x * 3 + 2] = raw[s + x * 3 + 0];
                }
            }
            Marshal.Copy(bgr, 0, bd.Scan0, bgr.Length);
            bmp.UnlockBits(bd);
            bmp.Save(pngPath, ImageFormat.Png);
        }
    }
}
"@

$fpid = (& $cli start | ConvertFrom-Json).process_id
$tempPpms = @()
$tag = 7000
try {
    for ($i = 0; $i -lt 40; $i++) {
        $prevEap = $ErrorActionPreference; $ErrorActionPreference = 'Continue'
        $raw = & $cli status $fpid 2>$null; $ec = $LASTEXITCODE
        $ErrorActionPreference = $prevEap
        if ($ec -eq 0 -and $raw) { $st0 = $raw | ConvertFrom-Json; if ($st0.ok -and $st0.present_calls_completed -gt 3) { break } }
        Start-Sleep -Milliseconds 250
    }

    & $cli set $fpid scene range | Out-Null
    & $cli set $fpid overlay on | Out-Null
    & $cli set $fpid pacer unlocked | Out-Null
    & $cli set $fpid async_gpu off | Out-Null
    & $cli set $fpid fixed_dt_us 4167 | Out-Null
    & $cli pause $fpid | Out-Null
    & $cli reset $fpid | Out-Null
    Start-Sleep -Milliseconds 80

    $tag++
    $c1 = (& $cli capture $fpid $tag 8 | ConvertFrom-Json)
    $tempPpms += $c1.path
    [RangeImgTools]::PpmToPng($c1.path, (Join-Path $reportDir 'range_hipfire.png'))

    & $cli set $fpid ads on | Out-Null
    $tag++
    $c2 = (& $cli capture $fpid $tag 60 | ConvertFrom-Json)
    $tempPpms += $c2.path
    [RangeImgTools]::PpmToPng($c2.path, (Join-Path $reportDir 'range_ads.png'))

    & $cli fire $fpid | Out-Null
    $tag++
    $c3 = (& $cli capture $fpid $tag 2 | ConvertFrom-Json)
    $tempPpms += $c3.path
    [RangeImgTools]::PpmToPng($c3.path, (Join-Path $reportDir 'range_fire_hitmarker.png'))
    $stHit = (& $cli status $fpid | ConvertFrom-Json)

    & $cli set $fpid ads off | Out-Null
    $tag++
    $c4 = (& $cli capture $fpid $tag 80 | ConvertFrom-Json)
    $tempPpms += $c4.path
    [RangeImgTools]::PpmToPng($c4.path, (Join-Path $reportDir 'range_target_knocked.png'))

    Write-Host ("`n=== TACTICAL RANGE + JOLT PHYSICS VERIFICATION ===") -ForegroundColor Cyan
    Write-Host ("  Scene={0}  HP={1}  Armor={2}  Ammo={3}/{4}  Shots={5}  Hits={6}  Headshots={7}  Score={8}  TargetsUp={9}/8" -f `
        $stHit.scene.kind, $stHit.tactical_range.hp, $stHit.tactical_range.armor, $stHit.tactical_range.ammo_in_mag, $stHit.tactical_range.ammo_reserve, `
        $stHit.tactical_range.shots_fired, $stHit.tactical_range.hits, $stHit.tactical_range.headshots, $stHit.tactical_range.score, $stHit.tactical_range.targets_up)

    if ($stHit.tactical_range.hits -lt 1 -or $stHit.tactical_range.targets_up -ge 8) {
        throw "Expected Jolt raycast bullet hit to knock down a target!"
    }

    & $cli reload $fpid | Out-Null
    & $cli step $fpid 600 | Out-Null
    $stReload = (& $cli status $fpid | ConvertFrom-Json)
    Write-Host ("  After Reload + 2.5s Flip-Up: Ammo={0}/{1}  TargetsUp={2}/8" -f $stReload.tactical_range.ammo_in_mag, $stReload.tactical_range.ammo_reserve, $stReload.tactical_range.targets_up)

    & $cli resume $fpid | Out-Null
    & $cli set $fpid fixed_dt_us 0 | Out-Null
    & $cli set $fpid async_gpu on | Out-Null
    Write-Host ("`n=== TACTICAL RANGE HARDWARE PBR PACING BENCHMARK ===") -ForegroundColor Cyan
    foreach ($res in @(
        @{ name='1080p'; w=1920; h=1080 },
        @{ name='1440p'; w=2560; h=1440 },
        @{ name='4K';    w=3840; h=2160 }
    )) {
        & $cli render_size $fpid $res.w $res.h | Out-Null
        Start-Sleep -Milliseconds 450
        $tr = (& $cli trace $fpid 120 | ConvertFrom-Json)
        $scn = $tr.summary.scene_gpu_us.median
        $prs = $tr.summary.present_gpu_us.median
        $fps = if ($tr.summary.cpu_present_interval_us.median -gt 0) { [math]::Round(1000000.0 / $tr.summary.cpu_present_interval_us.median, 1) } else { 0 }
        Write-Host ("  {0,-5} ({1}x{2})  scene_gpu={3,4}us  present_gpu={4,4}us  cadence={5,6} FPS  missed={6}" -f $res.name, $res.w, $res.h, $scn, $prs, $fps, $tr.summary.missed_deadlines)
    }

    Write-Host "`nSaved Tactical Range PNGs to: $reportDir" -ForegroundColor Green
} finally {
    & $cli stop $fpid | Out-Null
    Start-Sleep -Milliseconds 400
    foreach ($p in $tempPpms) { if ($p -and (Test-Path $p)) { Remove-Item $p -Force } }
}

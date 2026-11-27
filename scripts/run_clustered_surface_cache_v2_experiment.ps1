$ErrorActionPreference = 'Stop'
$repoRoot = Split-Path -Parent $PSScriptRoot
$cli = Join-Path $repoRoot 'build\Release\frame_warp_cli.exe'

Add-Type -TypeDefinition @"
using System;
using System.IO;

public static class V36Eval {
    public struct SeqMetrics {
        public double MeanPsnr;
        public double MeanSsim;
        public double TemporalRmse;
        public double P99;
    }

    static byte[] ReadPpmRgb(string path, out int w, out int h) {
        byte[] raw = File.ReadAllBytes(path);
        int pos = 0;
        Func<string> tok = () => {
            while (pos < raw.Length && (raw[pos] <= 32 || raw[pos] == '#')) {
                if (raw[pos] == '#') { while (pos < raw.Length && raw[pos] != '\n') pos++; }
                else pos++;
            }
            int s = pos;
            while (pos < raw.Length && raw[pos] > 32) pos++;
            return System.Text.Encoding.ASCII.GetString(raw, s, pos - s);
        };
        if (tok() != "P6") throw new Exception("Expected P6");
        w = int.Parse(tok());
        h = int.Parse(tok());
        tok();
        if (pos < raw.Length && raw[pos] <= 32) pos++;
        byte[] rgb = new byte[w * h * 3];
        Buffer.BlockCopy(raw, pos, rgb, 0, rgb.Length);
        return rgb;
    }

    public static SeqMetrics EvaluateSequence(string[] gtPaths, string[] testPaths) {
        int n = Math.Min(gtPaths.Length, testPaths.Length);
        double psnrSum = 0, ssimSum = 0, p99Sum = 0, tRmseSum = 0;
        int tCount = 0;
        byte[] prevGt = null, prevTest = null;
        for (int f = 0; f < n; f++) {
            int w, h, w2, h2;
            byte[] gt = ReadPpmRgb(gtPaths[f], out w, out h);
            byte[] ts = ReadPpmRgb(testPaths[f], out w2, out h2);
            int pix = w * h;
            double mseSum = 0;
            int[] hist = new int[256];
            for (int i = 0; i < pix * 3; i++) {
                int d = Math.Abs((int)gt[i] - (int)ts[i]);
                mseSum += d * d;
                hist[d]++;
            }
            double mse = mseSum / (pix * 3.0);
            psnrSum += mse <= 1e-9 ? 99.0 : 10.0 * Math.Log10(255.0 * 255.0 / mse);
            long target99 = (long)(pix * 3 * 0.99);
            long acc = 0;
            int p99 = 0;
            for (int b = 0; b < 256; b++) { acc += hist[b]; if (acc >= target99) { p99 = b; break; } }
            p99Sum += p99;
            double ssimAcc = 0; int blocks = 0;
            for (int by = 0; by + 8 <= h; by += 8) {
                for (int bx = 0; bx + 8 <= w; bx += 8) {
                    double mx = 0, my = 0, vx = 0, vy = 0, cov = 0;
                    for (int dy = 0; dy < 8; dy++) {
                        int row = (by + dy) * w + bx;
                        for (int dx = 0; dx < 8; dx++) {
                            int o = (row + dx) * 3;
                            double lx = 0.299 * gt[o] + 0.587 * gt[o + 1] + 0.114 * gt[o + 2];
                            double ly = 0.299 * ts[o] + 0.587 * ts[o + 1] + 0.114 * ts[o + 2];
                            mx += lx; my += ly;
                        }
                    }
                    mx /= 64.0; my /= 64.0;
                    for (int dy = 0; dy < 8; dy++) {
                        int row = (by + dy) * w + bx;
                        for (int dx = 0; dx < 8; dx++) {
                            int o = (row + dx) * 3;
                            double lx = (0.299 * gt[o] + 0.587 * gt[o + 1] + 0.114 * gt[o + 2]) - mx;
                            double ly = (0.299 * ts[o] + 0.587 * ts[o + 1] + 0.114 * ts[o + 2]) - my;
                            vx += lx * lx; vy += ly * ly; cov += lx * ly;
                        }
                    }
                    vx /= 63.0; vy /= 63.0; cov /= 63.0;
                    double c1 = 6.5025, c2 = 58.5225;
                    ssimAcc += ((2 * mx * my + c1) * (2 * cov + c2)) / ((mx * mx + my * my + c1) * (vx + vy + c2));
                    blocks++;
                }
            }
            ssimSum += ssimAcc / Math.Max(blocks, 1);
            if (prevGt != null && prevTest != null) {
                double tSum = 0;
                for (int i = 0; i < pix * 3; i++) {
                    double dg = (double)gt[i] - prevGt[i];
                    double dt = (double)ts[i] - prevTest[i];
                    double e = dt - dg;
                    tSum += e * e;
                }
                tRmseSum += Math.Sqrt(tSum / (pix * 3.0));
                tCount++;
            }
            prevGt = gt; prevTest = ts;
        }
        return new SeqMetrics {
            MeanPsnr = psnrSum / Math.Max(n, 1),
            MeanSsim = ssimSum / Math.Max(n, 1),
            TemporalRmse = tCount > 0 ? tRmseSum / tCount : 0,
            P99 = p99Sum / Math.Max(n, 1)
        };
    }
}
"@

$fpid = (& $cli start | ConvertFrom-Json).process_id
$tempPpms = @()
$tag = 9950

function Capture-Run($sel, $split, $surf, $clust, $mb) {
    & $cli pause $fpid | Out-Null
    for ($w = 0; $w -lt 20; $w++) { if ((& $cli status $fpid | ConvertFrom-Json).scene.paused) { break }; Start-Sleep -Milliseconds 15 }
    & $cli set $fpid selective_tiles $sel | Out-Null
    & $cli set $fpid tile_budget 15 | Out-Null
    & $cli set $fpid split_diff_spec $split | Out-Null
    & $cli set $fpid surface_cache $surf | Out-Null
    & $cli set $fpid clustered_lighting $clust | Out-Null
    & $cli set $fpid surface_cache_mb $mb | Out-Null
    & $cli reset $fpid | Out-Null
    for ($w = 0; $w -lt 20; $w++) { if ((& $cli status $fpid | ConvertFrom-Json).scene.time_ms -eq 0) { break }; Start-Sleep -Milliseconds 15 }
    & $cli set $fpid path occlusion_sweep | Out-Null
    for ($w = 0; $w -lt 20; $w++) { if ((& $cli status $fpid | ConvertFrom-Json).scene.camera_path -eq 'occlusion_sweep') { break }; Start-Sleep -Milliseconds 15 }

    $paths = @()
    $gpuSum = 0.0; $hitSum = 0.0; $collSum = 0.0; $lptSum = 0.0
    for ($f = 0; $f -lt 8; $f++) {
        $stepN = if ($f -eq 0) { 24 } else { 8 }
        $script:tag++
        $c = (& $cli capture $fpid $script:tag $stepN | ConvertFrom-Json)
        if (-not $c.ok) { throw "Capture failed" }
        $paths += $c.path
        $script:tempPpms += $c.path
        $st = (& $cli status $fpid | ConvertFrom-Json)
        if ($null -ne $st.timing.scene_gpu_us) { $gpuSum += $st.timing.scene_gpu_us }
        if ($st.selective_tiles.surface_cache) {
            $hitSum += $st.selective_tiles.surface_cache.hit_rate
            $collSum += $st.selective_tiles.surface_cache.collision_rate
            $lptSum += $st.selective_tiles.surface_cache.avg_active_lights_per_tile
        }
    }
    return @{
        paths = $paths
        avgGpu = [math]::Round($gpuSum / 8.0, 0)
        hitPct = [math]::Round(100.0 * $hitSum / 8.0, 1)
        collPct = [math]::Round(100.0 * $collSum / 8.0, 2)
        lightsPerTile = [math]::Round($lptSum / 8.0, 2)
    }
}

try {
    for ($i = 0; $i -lt 40; $i++) {
        $prev = $ErrorActionPreference; $ErrorActionPreference = 'Continue'
        $raw = & $cli status $fpid 2>$null; $ec = $LASTEXITCODE
        $ErrorActionPreference = $prev
        if ($ec -eq 0 -and $raw) { $st0 = $raw | ConvertFrom-Json; if ($st0.ok -and $st0.present_calls_completed -gt 3) { break } }
        Start-Sleep -Milliseconds 250
    }

    & $cli set $fpid scene range | Out-Null
    & $cli set $fpid overlay off | Out-Null
    & $cli set $fpid pacer unlocked | Out-Null
    & $cli set $fpid async_gpu off | Out-Null
    & $cli set $fpid fixed_dt_us 4167 | Out-Null
    & $cli set $fpid subtile_interleave on | Out-Null
    & $cli set $fpid auto_tile_budget off | Out-Null
    & $cli set $fpid cache_sharpen on | Out-Null
    & $cli set $fpid vrs off | Out-Null

    Write-Host "`n=== v0.36.0: CLUSTERED TILED LIGHTING + ROUGHNESS-AWARE SPECULAR + ATOMIC ADAPTIVE SURFACE CACHE ===" -ForegroundColor Cyan

    Write-Host "`n--- 1. Quality & GPU Cost Ablation (occlusion_sweep, 15% PAR-3, 1280x720) ---" -ForegroundColor Yellow
    $gt    = Capture-Run 'off' 'off' 'off' 'off' 4
    $uncl  = Capture-Run 'on'  'on'  'on'  'off' 4
    $cl4   = Capture-Run 'on'  'on'  'on'  'on'  4
    $cl16  = Capture-Run 'on'  'on'  'on'  'on'  16
    $cl64  = Capture-Run 'on'  'on'  'on'  'on'  64

    Write-Host ("  {0,-28} avg={1,4}us  [GROUND TRUTH]" -f 'Forward_1x1_GT', $gt.avgGpu)
    foreach ($row in @(
        @{ name='Split+Surf_4MiB (All 14L)';  r=$uncl },
        @{ name='Split+Surf_4MiB (Clustered)';r=$cl4 },
        @{ name='Split+Surf_16MiB(Clustered)';r=$cl16 },
        @{ name='Split+Surf_64MiB(Clustered)';r=$cl64 }
    )) {
        $ev = [V36Eval]::EvaluateSequence($gt.paths, $row.r.paths)
        Write-Host ("  {0,-28} avg={1,4}us  L/tile={2,5:N2}/14  hit={3,5:N1}%  coll={4,5:N2}%  PSNR={5,5:N2}dB  SSIM={6,6:N4}  tRMSE={7,4:N2}" -f `
            $row.name, $row.r.avgGpu, $row.r.lightsPerTile, $row.r.hitPct, $row.r.collPct, $ev.MeanPsnr, $ev.MeanSsim, $ev.TemporalRmse)
    }

    Write-Host "`n--- 2. Live Resolution Scaling (1080p / 1440p / 4K @ 120Hz: GT vs PAR3+Surf vs Split+Surf Clustered) ---" -ForegroundColor Yellow
    & $cli set $fpid fixed_dt_us 0 | Out-Null
    & $cli set $fpid pacer deadline | Out-Null
    & $cli set $fpid target_fps 120 | Out-Null
    & $cli set $fpid async_gpu on | Out-Null
    & $cli set $fpid tile_budget 15 | Out-Null
    & $cli set $fpid surface_cache_mb 16 | Out-Null
    & $cli set $fpid path corridor_sway | Out-Null
    & $cli resume $fpid | Out-Null
    Start-Sleep -Milliseconds 300

    foreach ($res in @(
        @{ name='1080p'; w=1920; h=1080 },
        @{ name='1440p'; w=2560; h=1440 },
        @{ name='4K';    w=3840; h=2160 }
    )) {
        & $cli render_size $fpid $res.w $res.h | Out-Null
        Start-Sleep -Milliseconds 300
        & $cli set $fpid selective_tiles off | Out-Null
        Start-Sleep -Milliseconds 300
        $trGt = (& $cli trace $fpid 55 | ConvertFrom-Json)

        & $cli set $fpid selective_tiles on | Out-Null
        & $cli set $fpid split_diff_spec off | Out-Null
        & $cli set $fpid surface_cache on | Out-Null
        Start-Sleep -Milliseconds 300
        $trPar = (& $cli trace $fpid 55 | ConvertFrom-Json)

        & $cli set $fpid split_diff_spec on | Out-Null
        & $cli set $fpid clustered_lighting off | Out-Null
        Start-Sleep -Milliseconds 300
        $trOff = (& $cli trace $fpid 55 | ConvertFrom-Json)

        & $cli set $fpid clustered_lighting on | Out-Null
        Start-Sleep -Milliseconds 300
        $trOn = (& $cli trace $fpid 55 | ConvertFrom-Json)
        $stOn = (& $cli status $fpid | ConvertFrom-Json)

        $vsGt = if ($trOn.summary.scene_gpu_us.median -gt 0) { [math]::Round($trGt.summary.scene_gpu_us.median / $trOn.summary.scene_gpu_us.median, 2) } else { 1.0 }
        $vsAll = if ($trOn.summary.scene_gpu_us.median -gt 0) { [math]::Round($trOff.summary.scene_gpu_us.median / $trOn.summary.scene_gpu_us.median, 2) } else { 1.0 }
        Write-Host ("  {0,-5} ({1}x{2}) | GT={3,4}us(m={4})  PAR3+SC={5,4}us(m={6})  Split14L={7,4}us(m={8})  SplitClust={9,4}us(m={10}) [{11}x vs GT, {12}x vs 14L]  L/tile={13}/14  hit={14}%  coll={15}%" -f `
            $res.name, $res.w, $res.h, `
            $trGt.summary.scene_gpu_us.median, $trGt.summary.missed_deadlines, `
            $trPar.summary.scene_gpu_us.median, $trPar.summary.missed_deadlines, `
            $trOff.summary.scene_gpu_us.median, $trOff.summary.missed_deadlines, `
            $trOn.summary.scene_gpu_us.median, $trOn.summary.missed_deadlines, `
            $vsGt, $vsAll, `
            $stOn.selective_tiles.surface_cache.avg_active_lights_per_tile, `
            [math]::Round(100.0 * $stOn.selective_tiles.surface_cache.hit_rate, 1), `
            [math]::Round(100.0 * $stOn.selective_tiles.surface_cache.collision_rate, 2))
    }
} finally {
    & $cli stop $fpid | Out-Null
    Start-Sleep -Milliseconds 400
    foreach ($p in $tempPpms) { if ($p -and (Test-Path $p)) { Remove-Item $p -Force } }
}

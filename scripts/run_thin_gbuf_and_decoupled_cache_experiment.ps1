$ErrorActionPreference = 'Stop'
$repoRoot = Split-Path -Parent $PSScriptRoot
$cli = Join-Path $repoRoot 'build\Release\frame_warp_cli.exe'

Add-Type -TypeDefinition @"
using System;
using System.IO;

public static class V37Eval {
    public struct SeqMetrics {
        public double MeanPsnr;
        public double MeanSsim;
        public double TemporalRmse;
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
        double psnrSum = 0, ssimSum = 0, tRmseSum = 0;
        int tCount = 0;
        byte[] prevGt = null, prevTest = null;
        for (int f = 0; f < n; f++) {
            int w, h, w2, h2;
            byte[] gt = ReadPpmRgb(gtPaths[f], out w, out h);
            byte[] ts = ReadPpmRgb(testPaths[f], out w2, out h2);
            int pix = w * h;
            double mseSum = 0;
            for (int i = 0; i < pix * 3; i++) {
                int d = (int)gt[i] - (int)ts[i];
                mseSum += d * d;
            }
            double mse = mseSum / (pix * 3.0);
            psnrSum += mse <= 1e-9 ? 99.0 : 10.0 * Math.Log10(255.0 * 255.0 / mse);
            double ssimAcc = 0; int blocks = 0;
            for (int by = 0; by + 8 <= h; by += 8) {
                for (int bx = 0; bx + 8 <= w; bx += 8) {
                    double mx = 0, my = 0, vx = 0, vy = 0, cov = 0;
                    for (int dy = 0; dy < 8; dy++) {
                        int row = (by + dy) * w + bx;
                        for (int dx = 0; dx < 8; dx++) {
                            int o = (row + dx) * 3;
                            mx += 0.299 * gt[o] + 0.587 * gt[o + 1] + 0.114 * gt[o + 2];
                            my += 0.299 * ts[o] + 0.587 * ts[o + 1] + 0.114 * ts[o + 2];
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
                    double e = ((double)ts[i] - prevTest[i]) - ((double)gt[i] - prevGt[i]);
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
            TemporalRmse = tCount > 0 ? tRmseSum / tCount : 0
        };
    }
}
"@

$fpid = (& $cli start | ConvertFrom-Json).process_id
$tempPpms = @()
$tag = 9700

function Capture-Run($sel, $split, $surf, $thin, $mb) {
    & $cli pause $fpid | Out-Null
    for ($w = 0; $w -lt 20; $w++) { if ((& $cli status $fpid | ConvertFrom-Json).scene.paused) { break }; Start-Sleep -Milliseconds 15 }
    & $cli set $fpid selective_tiles $sel | Out-Null
    & $cli set $fpid tile_budget 15 | Out-Null
    & $cli set $fpid split_diff_spec $split | Out-Null
    & $cli set $fpid surface_cache $surf | Out-Null
    & $cli set $fpid thin_gbuf $thin | Out-Null
    & $cli set $fpid clustered_lighting off | Out-Null
    & $cli set $fpid surface_cache_mb $mb | Out-Null
    & $cli reset $fpid | Out-Null
    for ($w = 0; $w -lt 20; $w++) { if ((& $cli status $fpid | ConvertFrom-Json).scene.time_ms -eq 0) { break }; Start-Sleep -Milliseconds 15 }
    & $cli set $fpid path occlusion_sweep | Out-Null
    for ($w = 0; $w -lt 20; $w++) { if ((& $cli status $fpid | ConvertFrom-Json).scene.camera_path -eq 'occlusion_sweep') { break }; Start-Sleep -Milliseconds 15 }

    $paths = @()
    $gpuSum = 0.0; $hitSum = 0.0; $collSum = 0.0
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
        }
    }
    return @{
        paths = $paths
        avgGpu = [math]::Round($gpuSum / 8.0, 0)
        hitPct = [math]::Round(100.0 * $hitSum / 8.0, 1)
        collPct = [math]::Round(100.0 * $collSum / 8.0, 2)
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

    Write-Host "`n=== v0.37.0: THIN MATERIAL G-BUFFER CACHE + NORMAL-DECOUPLED EMA SURFACE CACHE ===" -ForegroundColor Cyan

    Write-Host "`n--- 1. Quality & Cache Scaling (occlusion_sweep, 15% PAR-3, 1280x720) ---" -ForegroundColor Yellow
    $gt    = Capture-Run 'off' 'off' 'off' 'off' 4
    $vis4  = Capture-Run 'on'  'on'  'on'  'off' 4
    $th4   = Capture-Run 'on'  'on'  'on'  'on'  4
    $th16  = Capture-Run 'on'  'on'  'on'  'on'  16
    $th64  = Capture-Run 'on'  'on'  'on'  'on'  64

    Write-Host ("  {0,-28} avg={1,4}us  [GROUND TRUTH]" -f 'Forward_1x1_GT', $gt.avgGpu)
    foreach ($row in @(
        @{ name='Split+Surf_4MiB  (VisReEval)'; r=$vis4 },
        @{ name='Split+Surf_4MiB  (ThinGBuf)';  r=$th4 },
        @{ name='Split+Surf_16MiB (ThinGBuf)';  r=$th16 },
        @{ name='Split+Surf_64MiB (ThinGBuf)';  r=$th64 }
    )) {
        $ev = [V37Eval]::EvaluateSequence($gt.paths, $row.r.paths)
        Write-Host ("  {0,-28} avg={1,4}us  hit={2,5:N1}%  coll={3,5:N2}%  PSNR={4,5:N2}dB  SSIM={5,6:N4}  tRMSE={6,4:N2}" -f `
            $row.name, $row.r.avgGpu, $row.r.hitPct, $row.r.collPct, $ev.MeanPsnr, $ev.MeanSsim, $ev.TemporalRmse)
    }

    Write-Host "`n--- 2. Live 1080p / 1440p / 4K @ 120Hz: GT vs PAR3+SC vs Split(VisReEval) vs Split(ThinGBuf) ---" -ForegroundColor Yellow
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
        & $cli set $fpid thin_gbuf off | Out-Null
        Start-Sleep -Milliseconds 300
        $trVis = (& $cli trace $fpid 55 | ConvertFrom-Json)

        & $cli set $fpid thin_gbuf on | Out-Null
        Start-Sleep -Milliseconds 300
        $trThin = (& $cli trace $fpid 55 | ConvertFrom-Json)

        $vsGt = if ($trThin.summary.scene_gpu_us.median -gt 0) { [math]::Round($trGt.summary.scene_gpu_us.median / $trThin.summary.scene_gpu_us.median, 2) } else { 1.0 }
        $vsVis = if ($trThin.summary.scene_gpu_us.median -gt 0) { [math]::Round($trVis.summary.scene_gpu_us.median / $trThin.summary.scene_gpu_us.median, 2) } else { 1.0 }
        Write-Host ("  {0,-5} ({1}x{2}) | GT={3,4}us  PAR3+SC={4,4}us  SplitVis={5,4}us  SplitThin={6,4}us  [{7}x vs GT, {8}x vs VisReEval]" -f `
            $res.name, $res.w, $res.h, `
            $trGt.summary.scene_gpu_us.median, `
            $trPar.summary.scene_gpu_us.median, `
            $trVis.summary.scene_gpu_us.median, `
            $trThin.summary.scene_gpu_us.median, `
            $vsGt, $vsVis)
    }
} finally {
    & $cli stop $fpid | Out-Null
    Start-Sleep -Milliseconds 400
    foreach ($p in $tempPpms) { if ($p -and (Test-Path $p)) { Remove-Item $p -Force } }
}

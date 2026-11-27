$ErrorActionPreference = 'Stop'
$repoRoot = Split-Path -Parent $PSScriptRoot
$cli = Join-Path $repoRoot 'build\Release\frame_warp_cli.exe'

Add-Type -TypeDefinition @"
using System;
using System.IO;

public static class FourCeilingsEval {
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
$tag = 9800

function Capture-Feature($feat, $sel, $split, $surf, $quadOpt, $mb) {
    & $cli pause $fpid | Out-Null
    for ($w = 0; $w -lt 20; $w++) { if ((& $cli status $fpid | ConvertFrom-Json).scene.paused) { break }; Start-Sleep -Milliseconds 15 }
    & $cli set $fpid shader_feature $feat | Out-Null
    & $cli set $fpid selective_tiles $sel | Out-Null
    & $cli set $fpid tile_budget 15 | Out-Null
    & $cli set $fpid split_diff_spec $split | Out-Null
    & $cli set $fpid surface_cache $surf | Out-Null
    & $cli set $fpid thin_gbuf $quadOpt | Out-Null
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

    Write-Host "`n=== v0.37.0: FOUR TECHNICAL CEILINGS VERIFICATION SUITE ===" -ForegroundColor Cyan

    Write-Host "`n--- 1. Ceiling #1 & #2: Wave-Uniform Split Composite + Normal-Decoupled EMA Cache (720p occlusion_sweep) ---" -ForegroundColor Yellow
    $gt0   = Capture-Feature 'none' 'off' 'off' 'off' 'off' 4
    $full4 = Capture-Feature 'none' 'on'  'on'  'on'  'off' 4
    $opt4  = Capture-Feature 'none' 'on'  'on'  'on'  'on'  4
    $opt16 = Capture-Feature 'none' 'on'  'on'  'on'  'on'  16
    Write-Host ("  {0,-30} avg={1,4}us  [GROUND TRUTH]" -f 'Forward_1x1_GT', $gt0.avgGpu)
    foreach ($row in @(
        @{ name='Split+Surf_4MiB  (3-Ray Full)'; r=$full4 },
        @{ name='Split+Surf_4MiB  (WaveOpt)';    r=$opt4 },
        @{ name='Split+Surf_16MiB (WaveOpt)';    r=$opt16 }
    )) {
        $ev = [FourCeilingsEval]::EvaluateSequence($gt0.paths, $row.r.paths)
        Write-Host ("  {0,-30} avg={1,4}us  hit={2,5:N1}%  coll={3,5:N2}%  PSNR={4,5:N2}dB  SSIM={5,6:N4}  tRMSE={6,4:N2}" -f `
            $row.name, $row.r.avgGpu, $row.r.hitPct, $row.r.collPct, $ev.MeanPsnr, $ev.MeanSsim, $ev.TemporalRmse)
    }

    Write-Host "`n--- 2. Ceiling #3: Real-Game Stress (Deforming Mesh / Alpha Cutout / 16-Ray Multi-Bounce GI @ 720p) ---" -ForegroundColor Yellow
    foreach ($feat in @('deform', 'alpha_cutout', 'gi_bounce')) {
        $g = Capture-Feature $feat 'off' 'off' 'off' 'off' 16
        $u = Capture-Feature $feat 'on'  'off' 'on'  'off' 16
        $s = Capture-Feature $feat 'on'  'on'  'on'  'off' 16
        $evU = [FourCeilingsEval]::EvaluateSequence($g.paths, $u.paths)
        $evS = [FourCeilingsEval]::EvaluateSequence($g.paths, $s.paths)
        $spU = if ($u.avgGpu -gt 0) { [math]::Round($g.avgGpu / $u.avgGpu, 2) } else { 1.0 }
        $spS = if ($s.avgGpu -gt 0) { [math]::Round($g.avgGpu / $s.avgGpu, 2) } else { 1.0 }
        Write-Host ("  [{0,-12}] GT={1,4}us | PAR3+SC={2,4}us({3}x) PSNR={4,5:N2}dB SSIM={5,6:N4} tRMSE={6,4:N2} | Split+SC={7,4}us({8}x) PSNR={9,5:N2}dB SSIM={10,6:N4} tRMSE={11,4:N2}" -f `
            $feat, $g.avgGpu, $u.avgGpu, $spU, $evU.MeanPsnr, $evU.MeanSsim, $evU.TemporalRmse, `
            $s.avgGpu, $spS, $evS.MeanPsnr, $evS.MeanSsim, $evS.TemporalRmse)
    }

    Write-Host "`n--- 3. Ceiling #3 Killer Proof: 16-Ray Multi-Bounce GI Scaling (1080p / 1440p / 4K @ 120Hz) ---" -ForegroundColor Yellow
    & $cli set $fpid fixed_dt_us 0 | Out-Null
    & $cli set $fpid pacer deadline | Out-Null
    & $cli set $fpid target_fps 120 | Out-Null
    & $cli set $fpid async_gpu on | Out-Null
    & $cli set $fpid shader_feature gi_bounce | Out-Null
    & $cli set $fpid tile_budget 15 | Out-Null
    & $cli set $fpid surface_cache_mb 16 | Out-Null
    & $cli set $fpid thin_gbuf off | Out-Null
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
        $trGt = (& $cli trace $fpid 50 | ConvertFrom-Json)

        & $cli set $fpid selective_tiles on | Out-Null
        & $cli set $fpid split_diff_spec off | Out-Null
        & $cli set $fpid surface_cache on | Out-Null
        Start-Sleep -Milliseconds 300
        $trPar = (& $cli trace $fpid 50 | ConvertFrom-Json)

        & $cli set $fpid split_diff_spec on | Out-Null
        Start-Sleep -Milliseconds 300
        $trSplit = (& $cli trace $fpid 50 | ConvertFrom-Json)

        $spPar = if ($trPar.summary.scene_gpu_us.median -gt 0) { [math]::Round($trGt.summary.scene_gpu_us.median / $trPar.summary.scene_gpu_us.median, 2) } else { 1.0 }
        $spSplit = if ($trSplit.summary.scene_gpu_us.median -gt 0) { [math]::Round($trGt.summary.scene_gpu_us.median / $trSplit.summary.scene_gpu_us.median, 2) } else { 1.0 }
        Write-Host ("  {0,-5} GI | Forward_GT={1,5}us(m={2})  PAR3+SC={3,4}us(m={4}) [{5}x!]  Split+SC={6,4}us(m={7}) [{8}x!]" -f `
            $res.name, `
            $trGt.summary.scene_gpu_us.median, $trGt.summary.missed_deadlines, `
            $trPar.summary.scene_gpu_us.median, $trPar.summary.missed_deadlines, $spPar, `
            $trSplit.summary.scene_gpu_us.median, $trSplit.summary.missed_deadlines, $spSplit)
    }
} finally {
    & $cli stop $fpid | Out-Null
    Start-Sleep -Milliseconds 400
    foreach ($p in $tempPpms) { if ($p -and (Test-Path $p)) { Remove-Item $p -Force } }
}

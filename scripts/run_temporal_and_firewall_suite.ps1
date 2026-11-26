$ErrorActionPreference = 'Stop'
$repoRoot = Split-Path -Parent $PSScriptRoot
$cli = Join-Path $repoRoot 'build\Release\frame_warp_cli.exe'

if (-not (Test-Path $cli)) {
    throw "Release CLI not found at $cli. Build Release first."
}

Add-Type -TypeDefinition @"
using System;
using System.IO;
using System.Collections.Generic;

public static class TemporalSeqTools {
    public struct FrameMetrics {
        public double Psnr;
        public double Ssim;
        public double TempRmse;
        public double P99Err;
    }

    public struct SeqSummary {
        public double MeanPsnr;
        public double MinPsnr;
        public double StdPsnr;
        public double MeanSsim;
        public double MeanTempRmse;
        public double MaxTempRmse;
        public double MeanP99;
        public double DriftSlopePer100;
        public double EarlyPsnr;
        public double LatePsnr;
    }

    public static byte[] LoadPpm(string path, out int width, out int height) {
        byte[] raw = File.ReadAllBytes(path);
        int pos = 0;
        Func<string> nextToken = () => {
            while (pos < raw.Length && raw[pos] <= 32) pos++;
            int start = pos;
            while (pos < raw.Length && raw[pos] > 32) pos++;
            return System.Text.Encoding.ASCII.GetString(raw, start, pos - start);
        };
        if (nextToken() != "P6") throw new Exception("Expected P6 PPM");
        width = int.Parse(nextToken());
        height = int.Parse(nextToken());
        int.Parse(nextToken());
        pos++;
        byte[] pixels = new byte[width * height * 3];
        Buffer.BlockCopy(raw, pos, pixels, 0, pixels.Length);
        return pixels;
    }

    private static double ComputeSsim(double[] a, double[] b, int w, int h) {
        const double C1 = 6.5025, C2 = 58.5225;
        double sum = 0.0;
        int blocks = 0;
        for (int y = 0; y + 8 <= h; y += 8) {
            for (int x = 0; x + 8 <= w; x += 8) {
                double ma = 0, mb = 0;
                for (int dy = 0; dy < 8; dy++) {
                    int row = (y + dy) * w + x;
                    for (int dx = 0; dx < 8; dx++) { ma += a[row + dx]; mb += b[row + dx]; }
                }
                ma /= 64.0; mb /= 64.0;
                double va = 0, vb = 0, cov = 0;
                for (int dy = 0; dy < 8; dy++) {
                    int row = (y + dy) * w + x;
                    for (int dx = 0; dx < 8; dx++) {
                        double da = a[row + dx] - ma, db = b[row + dx] - mb;
                        va += da * da; vb += db * db; cov += da * db;
                    }
                }
                va /= 63.0; vb /= 63.0; cov /= 63.0;
                sum += ((2.0 * ma * mb + C1) * (2.0 * cov + C2)) / ((ma * ma + mb * mb + C1) * (va + vb + C2));
                blocks++;
            }
        }
        return blocks > 0 ? sum / blocks : 1.0;
    }

    public static SeqSummary EvaluateSequence(string[] refPaths, string[] testPaths, int[] frameIndices) {
        int count = refPaths.Length;
        double[] psnrs = new double[count];
        double[] ssims = new double[count];
        double[] p99s = new double[count];
        List<double> tempRmses = new List<double>();
        byte[] prevRef = null;
        byte[] prevTest = null;

        for (int k = 0; k < count; k++) {
            int w, h, tw, th;
            byte[] rp = LoadPpm(refPaths[k], out w, out h);
            byte[] tp = LoadPpm(testPaths[k], out tw, out th);
            int n = w * h;
            double[] rl = new double[n];
            double[] tl = new double[n];
            double mse = 0.0;
            int[] hist = new int[256];
            for (int i = 0; i < n; i++) {
                int p = i * 3;
                int dr = Math.Abs(rp[p] - tp[p]);
                int dg = Math.Abs(rp[p + 1] - tp[p + 1]);
                int db = Math.Abs(rp[p + 2] - tp[p + 2]);
                mse += (dr * dr + dg * dg + db * db) / 3.0;
                hist[(dr + dg + db) / 3]++;
                rl[i] = 0.2126 * rp[p] + 0.7152 * rp[p + 1] + 0.0722 * rp[p + 2];
                tl[i] = 0.2126 * tp[p] + 0.7152 * tp[p + 1] + 0.0722 * tp[p + 2];
            }
            mse /= n;
            psnrs[k] = mse < 1e-9 ? 99.99 : 10.0 * Math.Log10((255.0 * 255.0) / mse);
            ssims[k] = ComputeSsim(rl, tl, w, h);
            int target = (int)(n * 0.99), acc = 0, p99 = 0;
            for (int i = 0; i < 256; i++) { acc += hist[i]; if (acc >= target) { p99 = i; break; } }
            p99s[k] = p99;

            if (prevRef != null && prevTest != null) {
                double tMse = 0.0;
                int len = rp.Length;
                for (int i = 0; i < len; i++) {
                    double dRef = (double)rp[i] - (double)prevRef[i];
                    double dTest = (double)tp[i] - (double)prevTest[i];
                    double dd = dTest - dRef;
                    tMse += dd * dd;
                }
                tempRmses.Add(Math.Sqrt(tMse / len));
            }
            prevRef = rp;
            prevTest = tp;
        }

        double sumP = 0, minP = 999, sumS = 0, sumP99 = 0;
        for (int k = 0; k < count; k++) {
            sumP += psnrs[k];
            if (psnrs[k] < minP) minP = psnrs[k];
            sumS += ssims[k];
            sumP99 += p99s[k];
        }
        double meanP = sumP / count;
        double varP = 0;
        for (int k = 0; k < count; k++) { double d = psnrs[k] - meanP; varP += d * d; }

        double sumX = 0, sumY = 0, sumXX = 0, sumXY = 0;
        for (int k = 0; k < count; k++) {
            double x = frameIndices[k];
            double y = psnrs[k];
            sumX += x; sumY += y; sumXX += x * x; sumXY += x * y;
        }
        double denom = count * sumXX - sumX * sumX;
        double slopePerFrame = Math.Abs(denom) > 1e-9 ? (count * sumXY - sumX * sumY) / denom : 0.0;

        int third = Math.Max(1, count / 3);
        double earlySum = 0, lateSum = 0;
        for (int k = 0; k < third; k++) { earlySum += psnrs[k]; lateSum += psnrs[count - 1 - k]; }

        double sumT = 0, maxT = 0;
        foreach (double t in tempRmses) { sumT += t; if (t > maxT) maxT = t; }

        SeqSummary s = new SeqSummary();
        s.MeanPsnr = meanP;
        s.MinPsnr = minP;
        s.StdPsnr = Math.Sqrt(varP / count);
        s.MeanSsim = sumS / count;
        s.MeanTempRmse = tempRmses.Count > 0 ? sumT / tempRmses.Count : 0.0;
        s.MaxTempRmse = maxT;
        s.MeanP99 = sumP99 / count;
        s.DriftSlopePer100 = slopePerFrame * 100.0;
        s.EarlyPsnr = earlySum / third;
        s.LatePsnr = lateSum / third;
        return s;
    }
}
"@

$fpid = (& $cli start | ConvertFrom-Json).process_id
$tempPpms = @()
$tag = 2000
try {
    for ($i = 0; $i -lt 40; $i++) {
        $prevEap = $ErrorActionPreference
        $ErrorActionPreference = 'Continue'
        $raw = & $cli status $fpid 2>$null
        $ec = $LASTEXITCODE
        $ErrorActionPreference = $prevEap
        if ($ec -eq 0 -and $raw) {
            $st0 = $raw | ConvertFrom-Json
            if ($st0.ok -and $st0.present_calls_completed -gt 2) { break }
        }
        Start-Sleep -Milliseconds 250
    }

    & $cli pause $fpid | Out-Null
    & $cli set $fpid overlay off | Out-Null
    & $cli set $fpid async_gpu off | Out-Null
    & $cli set $fpid pacer unlocked | Out-Null
    & $cli set $fpid fixed_dt_us 4167 | Out-Null
    & $cli set $fpid stress 32 | Out-Null
    & $cli set $fpid mode full_render | Out-Null
    & $cli set $fpid divider 1 | Out-Null
    & $cli set $fpid tile_policy priority_stbn | Out-Null
    & $cli set $fpid cache_sharpen on | Out-Null
    & $cli set $fpid coarse_fallback on | Out-Null

    Write-Host "`n=== EXP A (Priority 6 + Priority 1): TEMPORAL VIDEO QUALITY HARNESS & PAR-3 SUB-TILE INTERLEAVING ===" -ForegroundColor Cyan
    Write-Host "    30 consecutive sampled frames across Frames 20..136 (every 4 frames @ 240Hz, zero paused contamination)" -ForegroundColor Gray
    $seqFrames = @()
    for ($f = 20; $f -le 136; $f += 4) { $seqFrames += $f }

    foreach ($scenePath in @('occlusion_sweep', 'fast_pan')) {
        Write-Host "`n--- Scene: $scenePath (30-frame motion sequence) ---" -ForegroundColor Yellow
        & $cli set $fpid path $scenePath | Out-Null

        & $cli set $fpid vrs off | Out-Null
        & $cli set $fpid selective_tiles off | Out-Null
        & $cli set $fpid subtile_interleave off | Out-Null
        & $cli reset $fpid | Out-Null
        Start-Sleep -Milliseconds 60
        $gtPaths = @()
        $prevF = 0
        foreach ($f in $seqFrames) {
            $tag++
            $cap = (& $cli capture $fpid $tag ($f - $prevF) | ConvertFrom-Json)
            $gtPaths += $cap.path
            $tempPpms += $cap.path
            $prevF = $f
        }

        $variants = @(
            @{ name='VRS_Tier2_foveated';    vrs='foveated'; tiles='off'; budget=100; inter='off' },
            @{ name='PAR_10pct_16x16_block'; vrs='off';      tiles='on';  budget=10;  inter='off' },
            @{ name='PAR3_10pct_4x4_inter';  vrs='off';      tiles='on';  budget=10;  inter='on'  },
            @{ name='PAR_15pct_16x16_block'; vrs='off';      tiles='on';  budget=15;  inter='off' },
            @{ name='PAR3_15pct_4x4_inter';  vrs='off';      tiles='on';  budget=15;  inter='on'  }
        )
        foreach ($v in $variants) {
            & $cli set $fpid vrs $v.vrs | Out-Null
            & $cli set $fpid selective_tiles $v.tiles | Out-Null
            & $cli set $fpid tile_budget $v.budget | Out-Null
            & $cli set $fpid subtile_interleave $v.inter | Out-Null
            & $cli reset $fpid | Out-Null
            Start-Sleep -Milliseconds 60
            $testPaths = @()
            $scnSum = 0.0
            $prevF = 0
            foreach ($f in $seqFrames) {
                $tag++
                $cap = (& $cli capture $fpid $tag ($f - $prevF) | ConvertFrom-Json)
                $testPaths += $cap.path
                $tempPpms += $cap.path
                $st = (& $cli status $fpid | ConvertFrom-Json)
                $scnSum += $st.timing.scene_gpu_us
                $prevF = $f
            }
            $sum = [TemporalSeqTools]::EvaluateSequence($gtPaths, $testPaths, $seqFrames)
            $avgScn = [math]::Round($scnSum / $seqFrames.Count, 0)
            Write-Host ("  {0,-22} avg_scn={1,4}us  PSNR={2,5:N2}dB(min={3,5:N2},sd={4:N2})  SSIM={5:N4}  tRMSE(flicker)={6,5:N2}  P99={7,4:N1}" -f $v.name, $avgScn, $sum.MeanPsnr, $sum.MinPsnr, $sum.StdPsnr, $sum.MeanSsim, $sum.MeanTempRmse, $sum.MeanP99)
        }
    }

    Write-Host "`n=== EXP B (Priority 2): PAR-5 HISTORY FIREWALL — 500-FRAME LONG-RUN DRIFT AUDIT ===" -ForegroundColor Cyan
    $driftFrames = @(40, 80, 120, 160, 200, 240, 280, 320, 360, 400, 440, 480, 500)
    & $cli set $fpid path fast_pan | Out-Null
    & $cli set $fpid vrs off | Out-Null
    & $cli set $fpid selective_tiles off | Out-Null
    & $cli set $fpid mode full_render | Out-Null
    & $cli set $fpid divider 1 | Out-Null
    & $cli reset $fpid | Out-Null
    Start-Sleep -Milliseconds 60
    $gtDrift = @()
    $prevF = 0
    foreach ($f in $driftFrames) {
        $tag++
        $cap = (& $cli capture $fpid $tag ($f - $prevF) | ConvertFrom-Json)
        $gtDrift += $cap.path
        $tempPpms += $cap.path
        $prevF = $f
    }

    foreach ($cfg in @(
        @{ name='PAR3_15pct_500f';         mode='full_render'; div=1; budget=15; inter='on' },
        @{ name='PAR3_15pct_Reproj4_500f'; mode='reproject';   div=4; budget=15; inter='on' }
    )) {
        & $cli set $fpid mode $cfg.mode | Out-Null
        & $cli set $fpid divider $cfg.div | Out-Null
        & $cli set $fpid selective_tiles on | Out-Null
        & $cli set $fpid tile_budget $cfg.budget | Out-Null
        & $cli set $fpid subtile_interleave $cfg.inter | Out-Null
        & $cli reset $fpid | Out-Null
        Start-Sleep -Milliseconds 60
        $testDrift = @()
        $prevF = 0
        foreach ($f in $driftFrames) {
            $tag++
            $cap = (& $cli capture $fpid $tag ($f - $prevF) | ConvertFrom-Json)
            $testDrift += $cap.path
            $tempPpms += $cap.path
            $prevF = $f
        }
        $sum = [TemporalSeqTools]::EvaluateSequence($gtDrift, $testDrift, $driftFrames)
        Write-Host ("  {0,-24} Mean={1,5:N2}dB  Early(F40-160)={2,5:N2}dB  Late(F380-500)={3,5:N2}dB  Slope={4,6:+0.00;-0.00;0.00}dB/100f  Min={5,5:N2}dB  tRMSE={6,4:N2}" -f $cfg.name, $sum.MeanPsnr, $sum.EarlyPsnr, $sum.LatePsnr, $sum.DriftSlopePer100, $sum.MinPsnr, $sum.MeanTempRmse)
    }

    Write-Host "`n=== EXP C (Priority 3): DAMAGE-RATE-AWARE SCHEDULER + stress=0 AUTO-BYPASS @ 1080p 120Hz ===" -ForegroundColor Cyan
    & $cli resize $fpid 1920 1080 | Out-Null
    & $cli set $fpid fixed_dt_us 0 | Out-Null
    & $cli set $fpid pacer deadline | Out-Null
    & $cli set $fpid target_fps 120 | Out-Null
    & $cli set $fpid async_gpu on | Out-Null
    & $cli set $fpid mode full_render | Out-Null
    & $cli set $fpid divider 1 | Out-Null
    & $cli set $fpid path occlusion_sweep | Out-Null
    & $cli set $fpid selective_tiles on | Out-Null
    & $cli set $fpid subtile_interleave on | Out-Null
    & $cli set $fpid tile_budget 100 | Out-Null
    & $cli set $fpid auto_tile_budget on | Out-Null
    & $cli resume $fpid | Out-Null

    foreach ($stage in @(
        @{ label='C1_stress0_bypass'; stress=0;  wait=1800 },
        @{ label='C2_stress64_hit';   stress=64; wait=2200 },
        @{ label='C3_stress64_hold';  stress=64; wait=1800 },
        @{ label='C4_stress0_recov';  stress=0;  wait=2800 }
    )) {
        & $cli set $fpid stress $stage.stress | Out-Null
        Start-Sleep -Milliseconds $stage.wait
        $st = (& $cli status $fpid | ConvertFrom-Json)
        $tr = (& $cli trace $fpid 100 summary | ConvertFrom-Json)
        $s = $tr.summary
        $t = $st.selective_tiles
        $critCov = if ($null -ne $s.critical_coverage_ratio) { "{0,5:N1}%" -f ($s.critical_coverage_ratio * 100.0) } else { "  n/a " }
        Write-Host ("  {0,-18} stress={1,2}  bypass={2,-5}  budget={3,3}%  dmg={4,2}%  scn_med={5,5}us(p95={6,5})  crit_cov={7}  misses={8}" -f $stage.label, $stage.stress, $t.par_bypass_active, $t.budget_pct, $t.smoothed_damage_pct, $s.scene_gpu_us.median, $s.scene_gpu_us.p95, $critCov, $s.missed_deadlines)
    }
} finally {
    & $cli stop $fpid | Out-Null
    Start-Sleep -Milliseconds 500
    foreach ($f in $tempPpms) { if ($f -and (Test-Path $f)) { Remove-Item $f -Force } }
}

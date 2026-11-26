$ErrorActionPreference = 'Stop'
$repoRoot = Split-Path -Parent $PSScriptRoot
$cli = Join-Path $repoRoot 'build\Release\frame_warp_cli.exe'
$reportDir = Join-Path $repoRoot 'build\Release\captures\split_surface_cache'
New-Item -ItemType Directory -Force -Path $reportDir | Out-Null

Add-Type -AssemblyName System.Drawing
Add-Type -ReferencedAssemblies System.Drawing -TypeDefinition @"
using System;
using System.Drawing;
using System.Drawing.Imaging;
using System.IO;
using System.Runtime.InteropServices;

public static class SplitCacheEval {
    public struct SeqMetrics {
        public double MeanPsnr;
        public double MinPsnr;
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

    public static void PpmToPng(string ppmPath, string pngPath) {
        int w, h;
        byte[] rgb = ReadPpmRgb(ppmPath, out w, out h);
        using (Bitmap bmp = new Bitmap(w, h, PixelFormat.Format24bppRgb)) {
            BitmapData bd = bmp.LockBits(new Rectangle(0, 0, w, h), ImageLockMode.WriteOnly, PixelFormat.Format24bppRgb);
            byte[] bgr = new byte[bd.Stride * h];
            for (int y = 0; y < h; y++) {
                int s = y * w * 3;
                int d = y * bd.Stride;
                for (int x = 0; x < w; x++) {
                    bgr[d + x * 3 + 0] = rgb[s + x * 3 + 2];
                    bgr[d + x * 3 + 1] = rgb[s + x * 3 + 1];
                    bgr[d + x * 3 + 2] = rgb[s + x * 3 + 0];
                }
            }
            Marshal.Copy(bgr, 0, bd.Scan0, bgr.Length);
            bmp.UnlockBits(bd);
            bmp.Save(pngPath, ImageFormat.Png);
        }
    }

    public static SeqMetrics EvaluateSequence(string[] gtPaths, string[] testPaths) {
        int n = Math.Min(gtPaths.Length, testPaths.Length);
        double psnrSum = 0, minPsnr = 999, ssimSum = 0, p99Sum = 0, tRmseSum = 0;
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
            double psnr = mse <= 1e-9 ? 99.0 : 10.0 * Math.Log10(255.0 * 255.0 / mse);
            psnrSum += psnr;
            if (psnr < minPsnr) minPsnr = psnr;
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
            MinPsnr = minPsnr,
            MeanSsim = ssimSum / Math.Max(n, 1),
            TemporalRmse = tCount > 0 ? tRmseSum / tCount : 0,
            P99 = p99Sum / Math.Max(n, 1)
        };
    }
}
"@

$fpid = (& $cli start | ConvertFrom-Json).process_id
$tempPpms = @()
$tag = 9800

function Capture-Run($pathMode, $sel, $vrs, $bud, $split, $surf) {
    & $cli pause $fpid | Out-Null
    for ($w = 0; $w -lt 20; $w++) { if ((& $cli status $fpid | ConvertFrom-Json).scene.paused) { break }; Start-Sleep -Milliseconds 15 }
    & $cli set $fpid selective_tiles $sel | Out-Null
    & $cli set $fpid vrs $vrs | Out-Null
    & $cli set $fpid tile_budget $bud | Out-Null
    & $cli set $fpid split_diff_spec $split | Out-Null
    & $cli set $fpid surface_cache $surf | Out-Null
    & $cli reset $fpid | Out-Null
    for ($w = 0; $w -lt 20; $w++) { if ((& $cli status $fpid | ConvertFrom-Json).scene.time_ms -eq 0) { break }; Start-Sleep -Milliseconds 15 }
    & $cli set $fpid path $pathMode | Out-Null
    for ($w = 0; $w -lt 20; $w++) { if ((& $cli status $fpid | ConvertFrom-Json).scene.camera_path -eq $pathMode) { break }; Start-Sleep -Milliseconds 15 }

    $paths = @()
    $gpuSum = 0.0; $shSum = 0.0; $totSum = 0.0
    for ($f = 0; $f -lt 8; $f++) {
        $stepN = if ($f -eq 0) { 24 } else { 8 }
        $script:tag++
        $c = (& $cli capture $fpid $script:tag $stepN | ConvertFrom-Json)
        if (-not $c.ok) { throw "Capture failed" }
        $paths += $c.path
        $script:tempPpms += $c.path
        $st = (& $cli status $fpid | ConvertFrom-Json)
        if ($null -ne $st.timing.scene_gpu_us) { $gpuSum += $st.timing.scene_gpu_us }
        if ($st.selective_tiles) { $shSum += $st.selective_tiles.shaded_tiles; $totSum += $st.selective_tiles.total_tiles }
    }
    return @{
        paths = $paths
        avgGpu = [math]::Round($gpuSum / 8.0, 0)
        shadedPct = if ($totSum -gt 0) { [math]::Round(100.0 * $shSum / $totSum, 1) } else { 100.0 }
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
    & $cli set $fpid shader_feature none | Out-Null

    Write-Host "`n=== PRIORITIES 5 & 6: SPLIT DIFFUSE/SPECULAR + PERSISTENT SURFACE-SPACE CACHE (14-LIGHT GUN RANGE) ===" -ForegroundColor Cyan

    foreach ($pm in @('corridor_sway', 'occlusion_sweep')) {
        Write-Host "`n--- Camera Path: $pm (1280x720, 8 frames F24..F80 @ 240Hz, 15% PAR-3 budget) ---" -ForegroundColor Yellow
        $gt     = Capture-Run $pm 'off' 'off'      100 'off' 'off'
        $vrs    = Capture-Run $pm 'off' 'foveated' 100 'off' 'off'
        $unif   = Capture-Run $pm 'on'  'off'       15 'off' 'off'
        $surf   = Capture-Run $pm 'on'  'off'       15 'off' 'on'
        $split  = Capture-Run $pm 'on'  'off'       15 'on'  'off'
        $both   = Capture-Run $pm 'on'  'off'       15 'on'  'on'

        if ($pm -eq 'corridor_sway') {
            [SplitCacheEval]::PpmToPng($unif.paths[4], (Join-Path $reportDir 'p56_unified_15p.png'))
            [SplitCacheEval]::PpmToPng($both.paths[4], (Join-Path $reportDir 'p56_split_surfcache_15p.png'))
        }

        Write-Host ("  {0,-24} avg={1,4}us  [GROUND TRUTH]" -f 'Forward_1x1_GT', $gt.avgGpu)
        foreach ($row in @(
            @{ name='VRS_Tier2_Fov';           r=$vrs },
            @{ name='PAR3_15p_Unified';        r=$unif },
            @{ name='PAR3_15p_SurfCache_Only'; r=$surf },
            @{ name='PAR3_15p_SplitDiffSpec';  r=$split },
            @{ name='PAR3_15p_Split+SurfCache';r=$both }
        )) {
            $ev = [SplitCacheEval]::EvaluateSequence($gt.paths, $row.r.paths)
            Write-Host ("  {0,-24} avg={1,4}us  shaded={2,5}%  PSNR={3,5:N2}dB(min={4,5:N2})  SSIM={5,6:N4}  tRMSE={6,5:N2}  P99={7,4:N1}" -f `
                $row.name, $row.r.avgGpu, $row.r.shadedPct, $ev.MeanPsnr, $ev.MinPsnr, $ev.MeanSsim, $ev.TemporalRmse, $ev.P99)
        }
    }
} finally {
    & $cli stop $fpid | Out-Null
    Start-Sleep -Milliseconds 400
    foreach ($p in $tempPpms) { if ($p -and (Test-Path $p)) { Remove-Item $p -Force } }
}

param(
    [switch]$KeepImages = $true,
    [switch]$OpenReportFolder = $false
)

$ErrorActionPreference = 'Stop'
$repoRoot = Split-Path -Parent $PSScriptRoot
$cli = Join-Path $repoRoot 'build\Release\frame_warp_cli.exe'
$reportDir = Join-Path $repoRoot 'build\Release\captures\shader_theories'

if (-not (Test-Path $cli)) {
    throw "Release CLI not found at $cli. Build Release first."
}

Add-Type -AssemblyName System.Drawing
Add-Type -ReferencedAssemblies System.Drawing -TypeDefinition @"
using System;
using System.Drawing;
using System.Drawing.Imaging;
using System.IO;
using System.Runtime.InteropServices;
using System.Collections.Generic;

public static class ShaderTheoryTools {
    public struct SeqSummary {
        public double MeanPsnr;
        public double MinPsnr;
        public double MeanSsim;
        public double MeanTempRmse;
        public double MeanP99;
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

    public static void SaveRgbAsPng(string ppmPath, string pngPath) {
        int w, h;
        byte[] rgb = LoadPpm(ppmPath, out w, out h);
        SaveBufferAsPng(rgb, w, h, pngPath);
    }

    public static void SaveAmplifiedDiffPng(string refPpmPath, string testPpmPath, string pngPath, int gain) {
        int rw, rh, tw, th;
        byte[] rp = LoadPpm(refPpmPath, out rw, out rh);
        byte[] tp = LoadPpm(testPpmPath, out tw, out th);
        byte[] diff = new byte[rw * rh * 3];
        for (int i = 0; i < diff.Length; i++) {
            int d = Math.Abs(rp[i] - tp[i]) * gain;
            diff[i] = (byte)(d > 255 ? 255 : d);
        }
        SaveBufferAsPng(diff, rw, rh, pngPath);
    }

    private static void SaveBufferAsPng(byte[] rgb, int w, int h, string pngPath) {
        using (Bitmap bmp = new Bitmap(w, h, PixelFormat.Format24bppRgb)) {
            BitmapData data = bmp.LockBits(new Rectangle(0, 0, w, h), ImageLockMode.WriteOnly, PixelFormat.Format24bppRgb);
            byte[] bgrRow = new byte[w * 3];
            for (int y = 0; y < h; y++) {
                int srcOff = y * w * 3;
                for (int x = 0; x < w; x++) {
                    bgrRow[x * 3 + 0] = rgb[srcOff + x * 3 + 2];
                    bgrRow[x * 3 + 1] = rgb[srcOff + x * 3 + 1];
                    bgrRow[x * 3 + 2] = rgb[srcOff + x * 3 + 0];
                }
                Marshal.Copy(bgrRow, 0, IntPtr.Add(data.Scan0, y * data.Stride), w * 3);
            }
            bmp.UnlockBits(data);
            bmp.Save(pngPath, ImageFormat.Png);
        }
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

    public static SeqSummary EvaluateSequence(string[] refPaths, string[] testPaths) {
        int count = refPaths.Length;
        double sumP = 0, minP = 999, sumS = 0, sumP99 = 0, sumT = 0;
        int tCount = 0;
        byte[] prevRef = null, prevTest = null;
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
            double psnr = mse < 1e-9 ? 99.99 : 10.0 * Math.Log10((255.0 * 255.0) / mse);
            sumP += psnr; if (psnr < minP) minP = psnr;
            sumS += ComputeSsim(rl, tl, w, h);
            int target = (int)(n * 0.99), acc = 0, p99 = 0;
            for (int i = 0; i < 256; i++) { acc += hist[i]; if (acc >= target) { p99 = i; break; } }
            sumP99 += p99;
            if (prevRef != null && prevTest != null) {
                double tMse = 0.0;
                for (int i = 0; i < rp.Length; i++) {
                    double dd = ((double)tp[i] - (double)prevTest[i]) - ((double)rp[i] - (double)prevRef[i]);
                    tMse += dd * dd;
                }
                sumT += Math.Sqrt(tMse / rp.Length);
                tCount++;
            }
            prevRef = rp; prevTest = tp;
        }
        SeqSummary s = new SeqSummary();
        s.MeanPsnr = sumP / count;
        s.MinPsnr = minP;
        s.MeanSsim = sumS / count;
        s.MeanTempRmse = tCount > 0 ? sumT / tCount : 0.0;
        s.MeanP99 = sumP99 / count;
        return s;
    }
}
"@

if ($KeepImages) { New-Item -ItemType Directory -Force -Path $reportDir | Out-Null }

$fpid = (& $cli start | ConvertFrom-Json).process_id
$tempPpms = @()
$tag = 3000
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
    & $cli set $fpid scene corridor | Out-Null
    & $cli set $fpid overlay off | Out-Null
    & $cli set $fpid async_gpu off | Out-Null
    & $cli set $fpid pacer unlocked | Out-Null
    & $cli set $fpid fixed_dt_us 4167 | Out-Null
    & $cli set $fpid pbr off | Out-Null
    & $cli set $fpid stress 16 | Out-Null
    & $cli set $fpid path occlusion_sweep | Out-Null
    & $cli set $fpid mode full_render | Out-Null
    & $cli set $fpid divider 1 | Out-Null
    & $cli set $fpid tile_policy priority_stbn | Out-Null
    & $cli set $fpid cache_sharpen on | Out-Null
    & $cli set $fpid coarse_fallback on | Out-Null
    & $cli set $fpid subtile_interleave on | Out-Null

    $seqFrames = @(24, 32, 40, 48, 56, 64, 72, 80, 88, 96, 104, 112)
    $shaders = @(
        @{ key='none';         theory='Baseline diffuse corridor';            save=$false },
        @{ key='hf_checker';   theory='1. Temporal resampling & detail';      save=$true  },
        @{ key='anim_uv';      theory='2. Material invalidation (UV scroll)'; save=$true  },
        @{ key='specular';     theory='3. View-dependent Blinn-Phong spec';   save=$true  },
        @{ key='moving_light'; theory='4. Global lighting propagation';       save=$true  },
        @{ key='emissive';     theory='5. Temporal contrast (pulsing neon)';  save=$true  },
        @{ key='hard_shadows'; theory='6. Nonlocal hard moving shadows';      save=$true  },
        @{ key='normal_map';   theory='7. High-freq normal-map stability';    save=$true  },
        @{ key='transparent';  theory='8. Multilayer transparent glass';      save=$true  },
        @{ key='all';          theory='ALL 8 SHADER FEATURES COMBINED';       save=$true  }
    )

    Write-Host "`n=== 8-SHADER THEORY STRESS SUITE (12-frame sequence F24..F112 @ 240Hz, stress=16, occlusion_sweep) ===" -ForegroundColor Cyan
    Write-Host "    Comparing 1x1 Native GT vs Hardware VRS Tier 2 vs Naive Geometry-Only PAR-3 (15%)" -ForegroundColor Gray

    foreach ($sh in $shaders) {
        & $cli set $fpid shader_feature $sh.key | Out-Null
        Write-Host ("`n--- {0,-13} | {1} ---" -f $sh.key, $sh.theory) -ForegroundColor Yellow

        & $cli set $fpid vrs off | Out-Null
        & $cli set $fpid selective_tiles off | Out-Null
        & $cli reset $fpid | Out-Null
        Start-Sleep -Milliseconds 60
        $gtPaths = @()
        $gtScn = 0.0
        $prevF = 0
        foreach ($f in $seqFrames) {
            $tag++
            $capRaw = & $cli capture $fpid $tag ($f - $prevF)
            if ($LASTEXITCODE -ne 0 -or -not $capRaw) {
                throw "Capture failed for shader '$($sh.key)' GT at frame $f (tag=$tag)"
            }
            $cap = ($capRaw | ConvertFrom-Json)
            if (-not $cap.ok -or -not $cap.path) {
                throw "Capture returned invalid response for shader '$($sh.key)' GT at frame $f"
            }
            $gtPaths += $cap.path
            $tempPpms += $cap.path
            $st = (& $cli status $fpid | ConvertFrom-Json)
            $gtScn += $st.timing.scene_gpu_us
            $prevF = $f
        }
        $gtAvg = [math]::Round($gtScn / $seqFrames.Count, 0)
        if ($KeepImages -and $sh.save) {
            [ShaderTheoryTools]::SaveRgbAsPng($gtPaths[-1], (Join-Path $reportDir "$($sh.key)_GT.png"))
        }
        Write-Host ("  GT_1x1_native    avg_scn={0,4}us  [GROUND TRUTH]" -f $gtAvg)

        foreach ($md in @(
            @{ name='VRS_Tier2_fov'; vrs='foveated'; tiles='off'; budget=100 },
            @{ name='PAR3_15pct';    vrs='off';      tiles='on';  budget=15  }
        )) {
            & $cli set $fpid vrs $md.vrs | Out-Null
            & $cli set $fpid selective_tiles $md.tiles | Out-Null
            & $cli set $fpid tile_budget $md.budget | Out-Null
            & $cli reset $fpid | Out-Null
            Start-Sleep -Milliseconds 60
            $testPaths = @()
            $scnSum = 0.0
            $prevF = 0
            foreach ($f in $seqFrames) {
                $tag++
                $capRaw = & $cli capture $fpid $tag ($f - $prevF)
                if ($LASTEXITCODE -ne 0 -or -not $capRaw) {
                    throw "Capture failed for shader '$($sh.key)' $($md.name) at frame $f (tag=$tag)"
                }
                $cap = ($capRaw | ConvertFrom-Json)
                if (-not $cap.ok -or -not $cap.path) {
                    throw "Capture returned invalid response for shader '$($sh.key)' $($md.name) at frame $f"
                }
                $testPaths += $cap.path
                $tempPpms += $cap.path
                $st = (& $cli status $fpid | ConvertFrom-Json)
                $scnSum += $st.timing.scene_gpu_us
                $prevF = $f
            }
            $avgScn = [math]::Round($scnSum / $seqFrames.Count, 0)
            $sum = [ShaderTheoryTools]::EvaluateSequence($gtPaths, $testPaths)
            if ($KeepImages -and $sh.save -and $md.name -eq 'PAR3_15pct') {
                [ShaderTheoryTools]::SaveRgbAsPng($testPaths[-1], (Join-Path $reportDir "$($sh.key)_PAR3.png"))
                [ShaderTheoryTools]::SaveAmplifiedDiffPng($gtPaths[-1], $testPaths[-1], (Join-Path $reportDir "$($sh.key)_PAR3_diff4x.png"), 4)
            }
            Write-Host ("  {0,-15}  avg_scn={1,4}us  PSNR={2,5:N2}dB(min={3,5:N2})  SSIM={4:N4}  tRMSE={5,5:N2}  P99={6,4:N1}" -f $md.name, $avgScn, $sum.MeanPsnr, $sum.MinPsnr, $sum.MeanSsim, $sum.MeanTempRmse, $sum.MeanP99)
        }
    }

    if ($KeepImages) {
        Write-Host "`nSaved all 8-shader GT, PAR-3, and 4x error-diff PNGs to: $reportDir" -ForegroundColor Green
        if ($OpenReportFolder) { Start-Process explorer.exe $reportDir }
    }
} finally {
    & $cli stop $fpid | Out-Null
    Start-Sleep -Milliseconds 500
    foreach ($f in $tempPpms) { if ($f -and (Test-Path $f)) { Remove-Item $f -Force } }
}

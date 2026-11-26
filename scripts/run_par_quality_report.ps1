param(
    [switch]$KeepImages = $true,
    [switch]$OpenReportFolder = $false
)

$ErrorActionPreference = 'Stop'
$repoRoot = Split-Path -Parent $PSScriptRoot
$cli = Join-Path $repoRoot 'build\Release\frame_warp_cli.exe'
$reportDir = Join-Path $repoRoot 'build\Release\captures\report'

if (-not (Test-Path $cli)) {
    throw "Release CLI not found at $cli. Build Release first: cmake --build build --config Release"
}

Add-Type -AssemblyName System.Drawing
Add-Type -ReferencedAssemblies System.Drawing -TypeDefinition @"
using System;
using System.Drawing;
using System.Drawing.Imaging;
using System.IO;
using System.Runtime.InteropServices;

public static class ParReportTools {
    public struct Metrics {
        public double FullPsnr;
        public double FoveaPsnr;
        public double FullSsim;
        public double FoveaSsim;
        public double SharpnessRatio;
        public double MeanAbsErr;
        public double P99AbsErr;
        public int MaxAbsErr;
    }

    private static byte[] LoadPpm(string path, out int width, out int height) {
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

    private static double ComputeSsim(double[] a, double[] b, int w, int h, int x0, int y0, int x1, int y1) {
        const double C1 = 6.5025;
        const double C2 = 58.5225;
        double sum = 0.0;
        int blocks = 0;
        for (int y = y0; y + 8 <= y1; y += 8) {
            for (int x = x0; x + 8 <= x1; x += 8) {
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
                        double da = a[row + dx] - ma;
                        double db = b[row + dx] - mb;
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

    private static double EdgeEnergy(double[] luma, int w, int h) {
        double e = 0.0;
        for (int y = 1; y < h - 1; y++) {
            int r0 = (y - 1) * w, r1 = y * w, r2 = (y + 1) * w;
            for (int x = 1; x < w - 1; x++) {
                double gx = -luma[r0 + x - 1] + luma[r0 + x + 1] - 2 * luma[r1 + x - 1] + 2 * luma[r1 + x + 1] - luma[r2 + x - 1] + luma[r2 + x + 1];
                double gy = -luma[r0 + x - 1] - 2 * luma[r0 + x] - luma[r0 + x + 1] + luma[r2 + x - 1] + 2 * luma[r2 + x] + luma[r2 + x + 1];
                e += Math.Sqrt(gx * gx + gy * gy);
            }
        }
        return e;
    }

    public static Metrics Compare(string refPath, string testPath) {
        int rw, rh, tw, th;
        byte[] rp = LoadPpm(refPath, out rw, out rh);
        byte[] tp = LoadPpm(testPath, out tw, out th);
        int n = rw * rh;
        double[] rl = new double[n];
        double[] tl = new double[n];
        double mseFull = 0.0, mseFov = 0.0, sumAe = 0.0;
        long fovPixels = 0;
        int fx0 = (int)(rw * 0.30), fx1 = (int)(rw * 0.70);
        int fy0 = (int)(rh * 0.30), fy1 = (int)(rh * 0.70);
        int[] hist = new int[256];
        int maxAe = 0;
        for (int y = 0; y < rh; y++) {
            bool inFy = y >= fy0 && y < fy1;
            for (int x = 0; x < rw; x++) {
                int idx = y * rw + x;
                int p = idx * 3;
                int dr = Math.Abs(rp[p] - tp[p]);
                int dg = Math.Abs(rp[p + 1] - tp[p + 1]);
                int db = Math.Abs(rp[p + 2] - tp[p + 2]);
                double sq = (dr * dr + dg * dg + db * db) / 3.0;
                mseFull += sq;
                if (inFy && x >= fx0 && x < fx1) { mseFov += sq; fovPixels++; }
                int ae = (dr + dg + db) / 3;
                hist[ae]++;
                sumAe += (dr + dg + db) / 3.0;
                int mx = Math.Max(dr, Math.Max(dg, db));
                if (mx > maxAe) maxAe = mx;
                rl[idx] = 0.2126 * rp[p] + 0.7152 * rp[p + 1] + 0.0722 * rp[p + 2];
                tl[idx] = 0.2126 * tp[p] + 0.7152 * tp[p + 1] + 0.0722 * tp[p + 2];
            }
        }
        mseFull /= n;
        mseFov /= Math.Max(1, fovPixels);
        int p99Target = (int)(n * 0.99), acc = 0, p99 = 0;
        for (int i = 0; i < 256; i++) { acc += hist[i]; if (acc >= p99Target) { p99 = i; break; } }
        double refEdge = EdgeEnergy(rl, rw, rh);
        double tstEdge = EdgeEnergy(tl, rw, rh);
        Metrics res = new Metrics();
        res.FullPsnr = mseFull < 1e-9 ? 99.99 : 10.0 * Math.Log10((255.0 * 255.0) / mseFull);
        res.FoveaPsnr = mseFov < 1e-9 ? 99.99 : 10.0 * Math.Log10((255.0 * 255.0) / mseFov);
        res.FullSsim = ComputeSsim(rl, tl, rw, rh, 0, 0, rw, rh);
        res.FoveaSsim = ComputeSsim(rl, tl, rw, rh, fx0, fy0, fx1, fy1);
        res.SharpnessRatio = refEdge > 1e-6 ? tstEdge / refEdge : 1.0;
        res.MeanAbsErr = sumAe / n;
        res.P99AbsErr = p99;
        res.MaxAbsErr = maxAe;
        return res;
    }
}
"@

if ($KeepImages) {
    New-Item -ItemType Directory -Force -Path $reportDir | Out-Null
}

$fpid = (& $cli start | ConvertFrom-Json).process_id
$tempPpms = @()
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

    Write-Host "`n=== 1. MATCHED-REFERENCE IMAGE QUALITY REPORT (Exact Frame #98 @ 240 Hz = 408,366 us) ===" -ForegroundColor Cyan
    & $cli pause $fpid | Out-Null
    & $cli set $fpid overlay off | Out-Null
    & $cli set $fpid async_gpu off | Out-Null
    & $cli set $fpid pacer unlocked | Out-Null
    & $cli set $fpid fixed_dt_us 4167 | Out-Null
    & $cli set $fpid stress 32 | Out-Null

    $tag = 500
    foreach ($scenePath in @('occlusion_sweep', 'fast_pan')) {
        Write-Host "`n--- Scene Path: $scenePath ---" -ForegroundColor Yellow
        $variants = @(
            @{ name='0_GT_native';           mode='full_render'; div=1; vrs='off';         tiles='off'; budget=100; policy='priority_stbn'; sharp='off'; save=$true  },
            @{ name='1_VRS_foveated';        mode='full_render'; div=1; vrs='foveated';    tiles='off'; budget=100; policy='priority_stbn'; sharp='off'; save=$true  },
            @{ name='2_VRS_2x2';             mode='full_render'; div=1; vrs='uniform_2x2'; tiles='off'; budget=100; policy='priority_stbn'; sharp='off'; save=$false },
            @{ name='3_FIFO_10pct';          mode='full_render'; div=1; vrs='off';         tiles='on';  budget=10;  policy='fifo';          sharp='off'; save=$false },
            @{ name='4_PAR_5pct_cliff';      mode='full_render'; div=1; vrs='off';         tiles='on';  budget=5;   policy='priority_stbn'; sharp='off'; save=$false },
            @{ name='5_PAR_10pct_bilin';     mode='full_render'; div=1; vrs='off';         tiles='on';  budget=10;  policy='priority_stbn'; sharp='off'; save=$false },
            @{ name='6_PAR_10pct_SHARP';     mode='full_render'; div=1; vrs='off';         tiles='on';  budget=10;  policy='priority_stbn'; sharp='on';  save=$false },
            @{ name='7_PAR_15pct_SHARP';     mode='full_render'; div=1; vrs='off';         tiles='on';  budget=15;  policy='priority_stbn'; sharp='on';  save=$true  },
            @{ name='8_REPROJ4_PAR15_SHARP'; mode='reproject';   div=4; vrs='off';         tiles='on';  budget=15;  policy='priority_stbn'; sharp='on';  save=$true  }
        )
        $gtPpm = $null
        foreach ($v in $variants) {
            $tag++
            & $cli set $fpid path $scenePath | Out-Null
            & $cli set $fpid mode $v.mode | Out-Null
            & $cli set $fpid divider $v.div | Out-Null
            & $cli set $fpid vrs $v.vrs | Out-Null
            & $cli set $fpid selective_tiles $v.tiles | Out-Null
            & $cli set $fpid tile_budget $v.budget | Out-Null
            & $cli set $fpid tile_policy $v.policy | Out-Null
            & $cli set $fpid cache_sharpen $v.sharp | Out-Null
            & $cli reset $fpid | Out-Null
            Start-Sleep -Milliseconds 120
            $cap = (& $cli capture $fpid $tag 98 | ConvertFrom-Json)
            $tempPpms += $cap.path
            $st = (& $cli status $fpid | ConvertFrom-Json)

            if ($v.name -eq '0_GT_native') {
                $gtPpm = $cap.path
                if ($KeepImages) {
                    [ParReportTools]::SaveRgbAsPng($cap.path, (Join-Path $reportDir "${scenePath}_0_GT_native.png"))
                }
                Write-Host ("{0,-22} scn={1,4}us  [GROUND TRUTH 1x1 REFERENCE]" -f $v.name, $st.timing.scene_gpu_us)
            } else {
                $m = [ParReportTools]::Compare($gtPpm, $cap.path)
                if ($KeepImages -and $v.save) {
                    [ParReportTools]::SaveRgbAsPng($cap.path, (Join-Path $reportDir "${scenePath}_$($v.name).png"))
                    [ParReportTools]::SaveAmplifiedDiffPng($gtPpm, $cap.path, (Join-Path $reportDir "${scenePath}_$($v.name)_diff4x.png"), 4)
                }
                Write-Host ("{0,-22} scn={1,4}us  PSNR={2,5:N2}dB(fov={3,5:N2})  SSIM={4:N4}  Sharp={5,5:N1}%  P99={6,2:N0}  Max={7,3}" -f $v.name, $st.timing.scene_gpu_us, $m.FullPsnr, $m.FoveaPsnr, $m.FullSsim, ($m.SharpnessRatio * 100.0), $m.P99AbsErr, $m.MaxAbsErr)
            }
        }
    }

    Write-Host "`n=== 2. CLOSED-LOOP AUTO TILE BUDGETER STRESS SHOCK (500 Hz Target = ~1,188 us) ===" -ForegroundColor Cyan
    & $cli set $fpid fixed_dt_us 0 | Out-Null
    & $cli set $fpid pacer deadline | Out-Null
    & $cli set $fpid target_fps 500 | Out-Null
    & $cli set $fpid async_gpu on | Out-Null
    & $cli set $fpid path fast_pan | Out-Null
    & $cli set $fpid mode full_render | Out-Null
    & $cli set $fpid divider 1 | Out-Null
    & $cli set $fpid vrs off | Out-Null
    & $cli set $fpid selective_tiles on | Out-Null
    & $cli set $fpid tile_policy priority_stbn | Out-Null
    & $cli set $fpid cache_sharpen on | Out-Null
    & $cli set $fpid tile_budget 100 | Out-Null
    & $cli set $fpid auto_tile_budget on | Out-Null
    & $cli resume $fpid | Out-Null

    foreach ($stage in @(
        @{ label='Stage1_stress0_idle';  stress=0;  wait=1600 },
        @{ label='Stage2_stress64_hit';  stress=64; wait=2200 },
        @{ label='Stage3_stress64_hold'; stress=64; wait=2200 },
        @{ label='Stage4_stress0_recov'; stress=0;  wait=2400 }
    )) {
        & $cli set $fpid stress $stage.stress | Out-Null
        Start-Sleep -Milliseconds $stage.wait
        $st = (& $cli status $fpid | ConvertFrom-Json)
        $tr = (& $cli trace $fpid 150 summary | ConvertFrom-Json)
        $s = $tr.summary
        Write-Host ("{0,-21} stress={1,2}  auto_budget={2,3}%  target={3,4}us  scn_med={4,4}us(p95={5,4})  crit_cov={6,5:N1}%" -f $stage.label, $stage.stress, $st.selective_tiles.budget_pct, $st.selective_tiles.target_scene_budget_us, $s.scene_gpu_us.median, $s.scene_gpu_us.p95, ($s.critical_coverage_ratio * 100))
    }

    if ($KeepImages) {
        Write-Host "`nSaved PNG comparisons + 4x error-diff images to:" -ForegroundColor Green
        Write-Host "  $reportDir"
        if ($OpenReportFolder) {
            Start-Process explorer.exe $reportDir
        }
    }
} finally {
    & $cli stop $fpid | Out-Null
    Start-Sleep -Milliseconds 500
    foreach ($f in $tempPpms) { if ($f -and (Test-Path $f)) { Remove-Item $f -Force } }
}

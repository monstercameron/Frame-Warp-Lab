param(
    [switch]$KeepImages = $true
)

$ErrorActionPreference = 'Stop'
$repoRoot = Split-Path -Parent $PSScriptRoot
$cli = Join-Path $repoRoot 'build\Release\frame_warp_cli.exe'
$reportDir = Join-Path $repoRoot 'build\Release\captures\v22_experiments'

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

public static class V22ImageTools {
    public struct Metrics {
        public double FullPsnr;
        public double FoveaPsnr;
        public double FullSsim;
        public double SharpnessRatio;
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
        const double C1 = 6.5025;
        const double C2 = 58.5225;
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
        double mseFull = 0.0, mseFov = 0.0;
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
                hist[(dr + dg + db) / 3]++;
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
        res.FullSsim = ComputeSsim(rl, tl, rw, rh);
        res.SharpnessRatio = refEdge > 1e-6 ? tstEdge / refEdge : 1.0;
        res.P99AbsErr = p99;
        res.MaxAbsErr = maxAe;
        return res;
    }
}
"@

if ($KeepImages) { New-Item -ItemType Directory -Force -Path $reportDir | Out-Null }

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

    & $cli pause $fpid | Out-Null
    & $cli set $fpid overlay off | Out-Null
    & $cli set $fpid async_gpu off | Out-Null
    & $cli set $fpid pacer unlocked | Out-Null
    & $cli set $fpid fixed_dt_us 4167 | Out-Null
    & $cli set $fpid stress 32 | Out-Null
    & $cli set $fpid mode full_render | Out-Null
    & $cli set $fpid divider 1 | Out-Null
    & $cli set $fpid vrs off | Out-Null
    & $cli set $fpid tile_policy priority_stbn | Out-Null

    Write-Host "`n=== EXP 1: 5% CLIFF RE-TEST (age=255 provenance fix + 4x4 coarse fallback, Frame #98 @ 240Hz) ===" -ForegroundColor Cyan
    $tag = 700
    foreach ($scenePath in @('fast_pan', 'occlusion_sweep')) {
        Write-Host "`n--- $scenePath ---" -ForegroundColor Yellow
        $gtPpm = $null
        $variants = @(
            @{ name='GT_1x1_native';        tiles='off'; budget=100; sharp='off'; coarse='off' },
            @{ name='PAR_5pct_agefix_only'; tiles='on';  budget=5;   sharp='on';  coarse='off' },
            @{ name='PAR_5pct_coarse_4x4';  tiles='on';  budget=5;   sharp='on';  coarse='on'  },
            @{ name='PAR_10pct_coarse_4x4'; tiles='on';  budget=10;  sharp='on';  coarse='on'  },
            @{ name='PAR_15pct_coarse_4x4'; tiles='on';  budget=15;  sharp='on';  coarse='on'  }
        )
        foreach ($v in $variants) {
            $tag++
            & $cli set $fpid path $scenePath | Out-Null
            & $cli set $fpid selective_tiles $v.tiles | Out-Null
            & $cli set $fpid tile_budget $v.budget | Out-Null
            & $cli set $fpid cache_sharpen $v.sharp | Out-Null
            & $cli set $fpid coarse_fallback $v.coarse | Out-Null
            & $cli reset $fpid | Out-Null
            Start-Sleep -Milliseconds 100
            $cap = (& $cli capture $fpid $tag 98 | ConvertFrom-Json)
            $tempPpms += $cap.path
            $st = (& $cli status $fpid | ConvertFrom-Json)
            if ($v.name -eq 'GT_1x1_native') {
                $gtPpm = $cap.path
                Write-Host ("{0,-22} scn={1,4}us  [GROUND TRUTH]" -f $v.name, $st.timing.scene_gpu_us)
            } else {
                $m = [V22ImageTools]::Compare($gtPpm, $cap.path)
                Write-Host ("{0,-22} scn={1,4}us  PSNR={2,5:N2}dB(fov={3,5:N2})  SSIM={4:N4}  Sharp={5,5:N1}%  P99={6,2:N0}  Max={7,3}" -f $v.name, $st.timing.scene_gpu_us, $m.FullPsnr, $m.FoveaPsnr, $m.FullSsim, ($m.SharpnessRatio * 100.0), $m.P99AbsErr, $m.MaxAbsErr)
            }
        }
    }

    Write-Host "`n=== EXP 2: HARD 100% CAMERA CUT RECOVERY CURVE (camera_cut t=1.63s, Cut+0..Cut+8 @ 240Hz, zero paused frames) ===" -ForegroundColor Cyan
    $offsets = @(0, 1, 2, 3, 4, 6, 8)
    $cutFrameBase = 392
    $gtCutPpms = @{}
    & $cli set $fpid path camera_cut | Out-Null
    & $cli set $fpid selective_tiles off | Out-Null
    & $cli set $fpid tile_budget 100 | Out-Null
    & $cli set $fpid cache_sharpen off | Out-Null
    & $cli set $fpid coarse_fallback off | Out-Null
    foreach ($off in $offsets) {
        $targetFrame = $cutFrameBase + $off
        $tag++
        & $cli reset $fpid | Out-Null
        Start-Sleep -Milliseconds 80
        $cap = (& $cli capture $fpid $tag $targetFrame | ConvertFrom-Json)
        $tempPpms += $cap.path
        $gtCutPpms[$off] = $cap.path
        if ($KeepImages -and $off -eq 0) {
            [V22ImageTools]::SaveRgbAsPng($cap.path, (Join-Path $reportDir "cut0_GT_native.png"))
        }
    }

    $cutConfigs = @(
        @{ name='PAR_15pct_NO_coarse';  budget=15; coarse='off' },
        @{ name='PAR_15pct_COARSE_4x4'; budget=15; coarse='on'  },
        @{ name='PAR_5pct_COARSE_4x4';  budget=5;  coarse='on'  }
    )
    foreach ($cfg in $cutConfigs) {
        Write-Host "`n--- $($cfg.name) ---" -ForegroundColor Yellow
        & $cli set $fpid selective_tiles on | Out-Null
        & $cli set $fpid tile_budget $cfg.budget | Out-Null
        & $cli set $fpid cache_sharpen on | Out-Null
        & $cli set $fpid coarse_fallback $cfg.coarse | Out-Null
        foreach ($off in $offsets) {
            $targetFrame = $cutFrameBase + $off
            $tag++
            & $cli reset $fpid | Out-Null
            Start-Sleep -Milliseconds 80
            $cap = (& $cli capture $fpid $tag $targetFrame | ConvertFrom-Json)
            $tempPpms += $cap.path
            $st = (& $cli status $fpid | ConvertFrom-Json)
            $m = [V22ImageTools]::Compare($gtCutPpms[$off], $cap.path)
            if ($KeepImages -and ($off -eq 0 -or $off -eq 3 -or $off -eq 6)) {
                [V22ImageTools]::SaveRgbAsPng($cap.path, (Join-Path $reportDir "cut${off}_$($cfg.name).png"))
            }
            Write-Host ("  Cut+{0,-2} (t={1,7}us)  scn={2,4}us  shaded={3,4}/{4,4}  PSNR={5,5:N2}dB(fov={6,5:N2})  SSIM={7:N4}  Sharp={8,5:N1}%  P99={9,3:N0}" -f $off, $cap.scene_time_us, $st.timing.scene_gpu_us, $st.selective_tiles.shaded_tiles, $st.selective_tiles.total_tiles, $m.FullPsnr, $m.FoveaPsnr, $m.FullSsim, ($m.SharpnessRatio * 100.0), $m.P99AbsErr)
        }
    }

    Write-Host "`n=== EXP 3: RESOLUTION SCALING ENVELOPE (720p -> 1080p -> 1440p, occlusion_sweep, 120 Hz) ===" -ForegroundColor Cyan
    & $cli set $fpid fixed_dt_us 0 | Out-Null
    & $cli set $fpid pacer deadline | Out-Null
    & $cli set $fpid target_fps 120 | Out-Null
    & $cli set $fpid async_gpu on | Out-Null
    & $cli set $fpid path occlusion_sweep | Out-Null
    & $cli set $fpid mode full_render | Out-Null
    & $cli set $fpid divider 1 | Out-Null
    & $cli resume $fpid | Out-Null

    $resolutions = @(
        @{ label='720p';  w=1280; h=720  },
        @{ label='1080p'; w=1920; h=1080 },
        @{ label='1440p'; w=2560; h=1440 }
    )
    foreach ($res in $resolutions) {
        $rz = (& $cli resize $fpid $res.w $res.h | ConvertFrom-Json)
        Start-Sleep -Milliseconds 300
        Write-Host ("`n--- {0} (actual client: {1}x{2}) ---" -f $res.label, $rz.client_size_px.width, $rz.client_size_px.height) -ForegroundColor Yellow
        foreach ($stress in @(0, 32, 64)) {
            & $cli set $fpid stress $stress | Out-Null
            $modes = @(
                @{ name='Native_1x1';    vrs='off';      tiles='off'; budget=100; sharp='off'; coarse='off' },
                @{ name='VRS_Tier2_fov'; vrs='foveated'; tiles='off'; budget=100; sharp='off'; coarse='off' },
                @{ name='PAR_15pct_ALL'; vrs='off';      tiles='on';  budget=15;  sharp='on';  coarse='on'  }
            )
            foreach ($md in $modes) {
                & $cli set $fpid vrs $md.vrs | Out-Null
                & $cli set $fpid selective_tiles $md.tiles | Out-Null
                & $cli set $fpid tile_budget $md.budget | Out-Null
                & $cli set $fpid cache_sharpen $md.sharp | Out-Null
                & $cli set $fpid coarse_fallback $md.coarse | Out-Null
                Start-Sleep -Milliseconds 1500
                $st = (& $cli status $fpid | ConvertFrom-Json)
                $tr = (& $cli trace $fpid 100 summary | ConvertFrom-Json)
                $s = $tr.summary
                $tilePct = if ($md.tiles -eq 'on' -and $st.selective_tiles.total_tiles -gt 0) { "{0,4:N1}%" -f (100.0 * $st.selective_tiles.shaded_tiles / $st.selective_tiles.total_tiles) } else { "100% " }
                Write-Host ("  stress={0,2}  {1,-14}  scn_med={2,5}us (p95={3,5})  pres={4,3}us  tiles={5}  misses={6}" -f $stress, $md.name, $s.scene_gpu_us.median, $s.scene_gpu_us.p95, $s.present_gpu_us.median, $tilePct, $s.missed_deadlines)
            }
        }
    }
} finally {
    & $cli stop $fpid | Out-Null
    Start-Sleep -Milliseconds 500
    foreach ($f in $tempPpms) { if ($f -and (Test-Path $f)) { Remove-Item $f -Force } }
}

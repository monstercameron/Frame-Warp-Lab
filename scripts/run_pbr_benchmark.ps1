param(
    [switch]$KeepImages = $true
)

$ErrorActionPreference = 'Stop'
$repoRoot = Split-Path -Parent $PSScriptRoot
$cli = Join-Path $repoRoot 'build\Release\frame_warp_cli.exe'
$reportDir = Join-Path $repoRoot 'build\Release\captures\pbr_benchmark'

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

public static class PbrBenchTools {
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
            while (pos < raw.Length && (raw[pos] <= 32 || raw[pos] == '#')) {
                if (raw[pos] == '#') { while (pos < raw.Length && raw[pos] != '\n') pos++; }
                else pos++;
            }
            int start = pos;
            while (pos < raw.Length && raw[pos] > 32) pos++;
            return System.Text.Encoding.ASCII.GetString(raw, start, pos - start);
        };
        string magic = nextToken();
        if (magic != "P6") throw new Exception("Expected P6 PPM");
        width = int.Parse(nextToken());
        height = int.Parse(nextToken());
        int maxv = int.Parse(nextToken());
        if (maxv != 255) throw new Exception("Expected maxval 255");
        if (pos < raw.Length && (raw[pos] == '\r' || raw[pos] == '\n' || raw[pos] == ' ' || raw[pos] == '\t')) pos++;
        int expected = width * height * 3;
        byte[] rgb = new byte[expected];
        Buffer.BlockCopy(raw, pos, rgb, 0, Math.Min(expected, raw.Length - pos));
        return rgb;
    }

    public static void SaveRgbAsPng(string ppmPath, string pngPath) {
        int w, h;
        byte[] rgb = LoadPpm(ppmPath, out w, out h);
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

    public static void SaveAmplifiedDiffPng(string refPpm, string testPpm, string pngPath, int gain) {
        int w1, h1, w2, h2;
        byte[] a = LoadPpm(refPpm, out w1, out h1);
        byte[] b = LoadPpm(testPpm, out w2, out h2);
        using (Bitmap bmp = new Bitmap(w1, h1, PixelFormat.Format24bppRgb)) {
            BitmapData bd = bmp.LockBits(new Rectangle(0, 0, w1, h1), ImageLockMode.WriteOnly, PixelFormat.Format24bppRgb);
            byte[] bgr = new byte[bd.Stride * h1];
            for (int y = 0; y < h1; y++) {
                int s = y * w1 * 3;
                int d = y * bd.Stride;
                for (int x = 0; x < w1; x++) {
                    int dr = Math.Min(255, Math.Abs(a[s + x * 3 + 0] - b[s + x * 3 + 0]) * gain);
                    int dg = Math.Min(255, Math.Abs(a[s + x * 3 + 1] - b[s + x * 3 + 1]) * gain);
                    int db = Math.Min(255, Math.Abs(a[s + x * 3 + 2] - b[s + x * 3 + 2]) * gain);
                    bgr[d + x * 3 + 0] = (byte)db;
                    bgr[d + x * 3 + 1] = (byte)dg;
                    bgr[d + x * 3 + 2] = (byte)dr;
                }
            }
            Marshal.Copy(bgr, 0, bd.Scan0, bgr.Length);
            bmp.UnlockBits(bd);
            bmp.Save(pngPath, ImageFormat.Png);
        }
    }

    static double ComputeSsimLuma(byte[] a, byte[] b, int w, int h) {
        double c1 = (0.01 * 255.0) * (0.01 * 255.0);
        double c2 = (0.03 * 255.0) * (0.03 * 255.0);
        double ssimSum = 0.0;
        int blocks = 0;
        for (int by = 0; by + 8 <= h; by += 8) {
            for (int bx = 0; bx + 8 <= w; bx += 8) {
                double mx = 0.0, my = 0.0;
                for (int y = 0; y < 8; y++) {
                    int row = ((by + y) * w + bx) * 3;
                    for (int x = 0; x < 8; x++) {
                        int i = row + x * 3;
                        mx += 0.2126 * a[i] + 0.7152 * a[i + 1] + 0.0722 * a[i + 2];
                        my += 0.2126 * b[i] + 0.7152 * b[i + 1] + 0.0722 * b[i + 2];
                    }
                }
                mx /= 64.0; my /= 64.0;
                double vx = 0.0, vy = 0.0, cov = 0.0;
                for (int y = 0; y < 8; y++) {
                    int row = ((by + y) * w + bx) * 3;
                    for (int x = 0; x < 8; x++) {
                        int i = row + x * 3;
                        double lx = (0.2126 * a[i] + 0.7152 * a[i + 1] + 0.0722 * a[i + 2]) - mx;
                        double ly = (0.2126 * b[i] + 0.7152 * b[i + 1] + 0.0722 * b[i + 2]) - my;
                        vx += lx * lx; vy += ly * ly; cov += lx * ly;
                    }
                }
                vx /= 63.0; vy /= 63.0; cov /= 63.0;
                ssimSum += ((2.0 * mx * my + c1) * (2.0 * cov + c2)) / ((mx * mx + my * my + c1) * (vx + vy + c2));
                blocks++;
            }
        }
        return blocks > 0 ? ssimSum / blocks : 0.0;
    }

    public static SeqSummary EvaluateSequence(string[] refPaths, string[] testPaths) {
        int count = Math.Min(refPaths.Length, testPaths.Length);
        double sumP = 0.0, minP = 999.0, sumS = 0.0, sumT = 0.0, sumP99 = 0.0;
        int tCount = 0;
        byte[] prevRef = null, prevTest = null;
        for (int f = 0; f < count; f++) {
            int w, h, w2, h2;
            byte[] rp = LoadPpm(refPaths[f], out w, out h);
            byte[] tp = LoadPpm(testPaths[f], out w2, out h2);
            int n = w * h;
            double mse = 0.0;
            int[] hist = new int[256];
            for (int i = 0; i < n; i++) {
                int p = i * 3;
                int dr = Math.Abs(rp[p] - tp[p]);
                int dg = Math.Abs(rp[p + 1] - tp[p + 1]);
                int db = Math.Abs(rp[p + 2] - tp[p + 2]);
                mse += dr * dr + dg * dg + db * db;
                hist[(dr + dg + db) / 3]++;
            }
            mse /= (n * 3.0);
            double psnr = mse <= 1e-9 ? 99.0 : 10.0 * Math.Log10((255.0 * 255.0) / mse);
            int p99Target = (int)(n * 0.99);
            int acc = 0, p99 = 0;
            for (int b = 0; b < 256; b++) { acc += hist[b]; if (acc >= p99Target) { p99 = b; break; } }
            sumP += psnr;
            if (psnr < minP) minP = psnr;
            sumS += ComputeSsimLuma(rp, tp, w, h);
            sumP99 += p99;
            if (prevRef != null && prevTest != null) {
                double tMse = 0.0;
                for (int i = 0; i < n * 3; i++) {
                    int dr = (rp[i] - prevRef[i]) - (tp[i] - prevTest[i]);
                    tMse += dr * dr;
                }
                sumT += Math.Sqrt(tMse / (n * 3.0));
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
$tag = 5000

function Capture-FrameStrict($p, $t, $steps) {
    $raw = & $cli capture $p $t $steps
    if ($LASTEXITCODE -ne 0 -or -not $raw) {
        throw "Capture failed (tag=$t steps=$steps)"
    }
    $obj = $raw | ConvertFrom-Json
    if (-not $obj.ok -or -not $obj.path) {
        throw "Invalid capture response (tag=$t)"
    }
    return $obj.path
}

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
    & $cli set $fpid stress 0 | Out-Null
    & $cli set $fpid path occlusion_sweep | Out-Null
    & $cli set $fpid mode full_render | Out-Null
    & $cli set $fpid divider 1 | Out-Null
    & $cli set $fpid tile_policy priority_stbn | Out-Null
    & $cli set $fpid cache_sharpen on | Out-Null
    & $cli set $fpid coarse_fallback on | Out-Null
    & $cli set $fpid subtile_interleave on | Out-Null
    & $cli set $fpid shader_feature none | Out-Null
    & $cli set $fpid pbr on | Out-Null

    $seqFrames = @(24, 32, 40, 48, 56, 64, 72, 80, 88, 96, 104, 112)

    Write-Host "`n=== PART 1: FULL PBR + TEXTURING QUALITY (1280x720, stress=0, F24..F112 @ 240Hz) ===" -ForegroundColor Cyan
    Write-Host "    Cook-Torrance GGX + Kulla-Conty Multiscatter + Clearcoat + Split-Sum IBL + 16x Aniso Mipmaps + ACES" -ForegroundColor Gray

    foreach ($aa in @('on', 'off')) {
        & $cli set $fpid aniso_mip $aa | Out-Null
        $label = if ($aa -eq 'on') { '16x Aniso SampleGrad + Toksvig + Specular AA' } else { 'Naive Mip-0 (No Mipmaps / No Specular AA)' }
        Write-Host "`n--- aniso_mip=$aa ($label) ---" -ForegroundColor Yellow

        & $cli set $fpid vrs off | Out-Null
        & $cli set $fpid selective_tiles off | Out-Null
        & $cli reset $fpid | Out-Null
        Start-Sleep -Milliseconds 60
        $gtPaths = @()
        $gtScn = 0.0
        $prevF = 0
        foreach ($f in $seqFrames) {
            $tag++
            $p = Capture-FrameStrict $fpid $tag ($f - $prevF)
            $gtPaths += $p
            $tempPpms += $p
            $st = (& $cli status $fpid | ConvertFrom-Json)
            $gtScn += $st.timing.scene_gpu_us
            $prevF = $f
        }
        $gtAvg = [math]::Round($gtScn / $seqFrames.Count, 0)
        if ($KeepImages -and $aa -eq 'on') {
            [PbrBenchTools]::SaveRgbAsPng($gtPaths[-1], (Join-Path $reportDir 'pbr_gt_720p.png'))
        }
        Write-Host ("  GT_1x1_native    avg_scn={0,4}us  [GROUND TRUTH]" -f $gtAvg)

        foreach ($md in @(
            @{ name='VRS_Tier2_fov'; vrs='foveated'; tiles='off'; budget=100 },
            @{ name='PAR3_15pct';    vrs='off';      tiles='on';  budget=15  },
            @{ name='PAR3_25pct';    vrs='off';      tiles='on';  budget=25  }
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
                $p = Capture-FrameStrict $fpid $tag ($f - $prevF)
                $testPaths += $p
                $tempPpms += $p
                $st = (& $cli status $fpid | ConvertFrom-Json)
                $scnSum += $st.timing.scene_gpu_us
                $prevF = $f
            }
            $avgScn = [math]::Round($scnSum / $seqFrames.Count, 0)
            $sum = [PbrBenchTools]::EvaluateSequence($gtPaths, $testPaths)
            if ($KeepImages -and $aa -eq 'on') {
                if ($md.name -eq 'PAR3_15pct') {
                    [PbrBenchTools]::SaveRgbAsPng($testPaths[-1], (Join-Path $reportDir 'pbr_par3_15pct.png'))
                    [PbrBenchTools]::SaveAmplifiedDiffPng($gtPaths[-1], $testPaths[-1], (Join-Path $reportDir 'pbr_par3_15pct_diff4x.png'), 4)
                } elseif ($md.name -eq 'VRS_Tier2_fov') {
                    [PbrBenchTools]::SaveAmplifiedDiffPng($gtPaths[-1], $testPaths[-1], (Join-Path $reportDir 'pbr_vrs_diff4x.png'), 4)
                }
            }
            Write-Host ("  {0,-15}  avg_scn={1,4}us  PSNR={2,5:N2}dB(min={3,5:N2})  SSIM={4:N4}  tRMSE={5,5:N2}  P99={6,4:N1}" -f $md.name, $avgScn, $sum.MeanPsnr, $sum.MinPsnr, $sum.MeanSsim, $sum.MeanTempRmse, $sum.MeanP99)
        }
    }

    Write-Host "`n=== PART 2: PURE PBR MULTI-RESOLUTION SCALING (1080p, 1440p, 4K, stress=0) ===" -ForegroundColor Cyan
    & $cli set $fpid aniso_mip on | Out-Null
    & $cli set $fpid pbr on | Out-Null
    & $cli resume $fpid | Out-Null
    & $cli set $fpid fixed_dt_us 0 | Out-Null

    foreach ($res in @(
        @{ name='1080p'; w=1920; h=1080 },
        @{ name='1440p'; w=2560; h=1440 },
        @{ name='4K';    w=3840; h=2160 }
    )) {
        & $cli render_size $fpid $res.w $res.h | Out-Null
        Start-Sleep -Milliseconds 120

        & $cli set $fpid vrs off | Out-Null
        & $cli set $fpid selective_tiles off | Out-Null
        Start-Sleep -Milliseconds 450
        $trGt = (& $cli trace $fpid 120 | ConvertFrom-Json)
        $gtMed = $trGt.summary.scene_gpu_us.median

        & $cli set $fpid vrs foveated | Out-Null
        & $cli set $fpid selective_tiles off | Out-Null
        Start-Sleep -Milliseconds 450
        $trVrs = (& $cli trace $fpid 120 | ConvertFrom-Json)
        $vrsMed = $trVrs.summary.scene_gpu_us.median

        & $cli set $fpid vrs off | Out-Null
        & $cli set $fpid selective_tiles on | Out-Null
        & $cli set $fpid tile_budget 15 | Out-Null
        Start-Sleep -Milliseconds 450
        $trPar = (& $cli trace $fpid 120 | ConvertFrom-Json)
        $parMed = $trPar.summary.scene_gpu_us.median

        $spd = if ($parMed -gt 0) { [math]::Round($gtMed / $parMed, 2) } else { 0 }
        Write-Host ("  {0,-5} ({1}x{2})  GT={3,5}us | VRS={4,5}us | PAR3_15%={5,5}us  ({6}x vs GT)" -f $res.name, $res.w, $res.h, $gtMed, $vrsMed, $parMed, $spd)
    }

    if ($KeepImages) {
        Write-Host "`nSaved PBR captures to: $reportDir" -ForegroundColor Green
    }
} finally {
    & $cli stop $fpid | Out-Null
    Start-Sleep -Milliseconds 500
    foreach ($f in $tempPpms) { if ($f -and (Test-Path $f)) { Remove-Item $f -Force } }
}

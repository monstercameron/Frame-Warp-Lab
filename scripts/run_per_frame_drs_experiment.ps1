$ErrorActionPreference = 'Stop'
$repoRoot = Split-Path -Parent $PSScriptRoot
$cli = Join-Path $repoRoot 'build\Release\frame_warp_cli.exe'

if (-not (Test-Path $cli)) {
    throw "Release CLI not found at $cli. Build Release first."
}

Add-Type -TypeDefinition @"
using System;
using System.IO;
public static class DrsMetricTools {
    public struct Metrics {
        public double FullPsnr;
        public double FullSsim;
        public double P99AbsErr;
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
    public static Metrics Compare(string refPath, string testPath) {
        int rw, rh, tw, th;
        byte[] rp = LoadPpm(refPath, out rw, out rh);
        byte[] tp = LoadPpm(testPath, out tw, out th);
        int n = rw * rh;
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
        int p99Target = (int)(n * 0.99), acc = 0, p99 = 0;
        for (int i = 0; i < 256; i++) { acc += hist[i]; if (acc >= p99Target) { p99 = i; break; } }
        double ssimSum = 0.0; int blocks = 0;
        for (int y = 0; y + 8 <= rh; y += 8) {
            for (int x = 0; x + 8 <= rw; x += 8) {
                double ma = 0, mb = 0;
                for (int dy = 0; dy < 8; dy++) for (int dx = 0; dx < 8; dx++) { ma += rl[(y+dy)*rw+x+dx]; mb += tl[(y+dy)*rw+x+dx]; }
                ma /= 64.0; mb /= 64.0;
                double va = 0, vb = 0, cov = 0;
                for (int dy = 0; dy < 8; dy++) for (int dx = 0; dx < 8; dx++) {
                    double da = rl[(y+dy)*rw+x+dx] - ma, db = tl[(y+dy)*rw+x+dx] - mb;
                    va += da*da; vb += db*db; cov += da*db;
                }
                va /= 63.0; vb /= 63.0; cov /= 63.0;
                ssimSum += ((2*ma*mb + 6.5025)*(2*cov + 58.5225))/((ma*ma+mb*mb+6.5025)*(va+vb+58.5225));
                blocks++;
            }
        }
        Metrics m = new Metrics();
        m.FullPsnr = mse < 1e-9 ? 99.99 : 10.0 * Math.Log10((255.0 * 255.0) / mse);
        m.FullSsim = blocks > 0 ? ssimSum / blocks : 1.0;
        m.P99AbsErr = p99;
        return m;
    }
}
"@

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

    & $cli resize $fpid 1920 1080 | Out-Null
    Start-Sleep -Milliseconds 250

    Write-Host "`n=== EXP 1: ZERO-ALLOCATION PER-FRAME DRS SWEEP (50% <-> 100% EVERY FRAME @ 1080p 120Hz) ===" -ForegroundColor Cyan
    & $cli set $fpid pacer deadline | Out-Null
    & $cli set $fpid target_fps 120 | Out-Null
    & $cli set $fpid async_gpu on | Out-Null
    & $cli set $fpid path occlusion_sweep | Out-Null
    & $cli set $fpid mode full_render | Out-Null
    & $cli set $fpid divider 1 | Out-Null
    & $cli set $fpid stress 32 | Out-Null
    & $cli set $fpid upscale_filter catmull_rom | Out-Null
    & $cli set $fpid drs_min 50 | Out-Null
    & $cli set $fpid drs_max 100 | Out-Null
    & $cli set $fpid cache_sharpen on | Out-Null
    & $cli set $fpid coarse_fallback on | Out-Null

    foreach ($cfg in @(
        @{ name='Sweep_Native_50_100'; tiles='off' },
        @{ name='Sweep_PAR15_50_100';  tiles='on'  }
    )) {
        & $cli set $fpid selective_tiles $cfg.tiles | Out-Null
        & $cli set $fpid tile_budget 15 | Out-Null
        $before = (& $cli status $fpid | ConvertFrom-Json).offscreen_render
        & $cli set $fpid drs sweep | Out-Null
        Start-Sleep -Milliseconds 1600
        $after = (& $cli status $fpid | ConvertFrom-Json).offscreen_render
        $tr = (& $cli trace $fpid 120 summary | ConvertFrom-Json)
        $s = $tr.summary
        $dChanges = $after.scale_changes - $before.scale_changes
        $dReallocs = $after.reallocations - $before.reallocations
        Write-Host ("  {0,-21} scale=[{1}%..{2}%]  changes={3,3}  reallocs={4}  scn_med={5,4}us(p95={6,4})  ft_med={7,4}us  misses={8}" -f $cfg.name, $s.render_scale_pct.min_us, $s.render_scale_pct.max_us, $dChanges, $dReallocs, $s.scene_gpu_us.median, $s.scene_gpu_us.p95, $s.cpu_present_interval_us.median, $s.missed_deadlines)
        & $cli set $fpid drs off | Out-Null
    }

    Write-Host "`n=== EXP 1B: EXACT FRAME #98 QUALITY UNDER LIVE PER-FRAME SWEEP (PAR Temporal Cache Survival) ===" -ForegroundColor Cyan
    & $cli pause $fpid | Out-Null
    & $cli set $fpid overlay off | Out-Null
    & $cli set $fpid async_gpu off | Out-Null
    & $cli set $fpid pacer unlocked | Out-Null
    & $cli set $fpid fixed_dt_us 4167 | Out-Null
    & $cli set $fpid stress 32 | Out-Null
    & $cli set $fpid drs off | Out-Null
    & $cli set $fpid render_scale 100 | Out-Null
    & $cli set $fpid selective_tiles off | Out-Null
    & $cli reset $fpid | Out-Null
    Start-Sleep -Milliseconds 100
    $gt = (& $cli capture $fpid 1101 98 | ConvertFrom-Json)
    $tempPpms += $gt.path

    foreach ($cfg in @(
        @{ name='Sweep_Native_Frame98'; tiles='off'; tag=1102 },
        @{ name='Sweep_PAR15_Frame98';  tiles='on';  tag=1103 }
    )) {
        & $cli set $fpid selective_tiles $cfg.tiles | Out-Null
        & $cli set $fpid tile_budget 15 | Out-Null
        & $cli set $fpid render_scale 100 | Out-Null
        & $cli set $fpid drs sweep | Out-Null
        & $cli reset $fpid | Out-Null
        Start-Sleep -Milliseconds 100
        $cap = (& $cli capture $fpid $cfg.tag 98 | ConvertFrom-Json)
        $tempPpms += $cap.path
        $st = (& $cli status $fpid | ConvertFrom-Json)
        $m = [DrsMetricTools]::Compare($gt.path, $cap.path)
        Write-Host ("  {0,-21} active={1}x{2} ({3}%)  scn={4,4}us  PSNR={5,5:N2}dB  SSIM={6:N4}  P99={7,2:N0}  reallocs={8}" -f $cfg.name, $st.offscreen_render.width, $st.offscreen_render.height, $st.offscreen_render.scale_pct, $st.timing.scene_gpu_us, $m.FullPsnr, $m.FullSsim, $m.P99AbsErr, $st.offscreen_render.reallocations)
    }

    Write-Host "`n=== EXP 2: CLOSED-LOOP AUTO PER-FRAME DRS UNDER STRESS SHOCK (1080p 120Hz, stress 0 -> 64 -> 0) ===" -ForegroundColor Cyan
    & $cli set $fpid fixed_dt_us 0 | Out-Null
    & $cli set $fpid pacer deadline | Out-Null
    & $cli set $fpid target_fps 120 | Out-Null
    & $cli set $fpid async_gpu on | Out-Null
    & $cli set $fpid selective_tiles off | Out-Null
    & $cli set $fpid render_scale 100 | Out-Null
    & $cli set $fpid drs_min 50 | Out-Null
    & $cli set $fpid drs_max 100 | Out-Null
    & $cli set $fpid drs auto | Out-Null
    & $cli resume $fpid | Out-Null

    $reallocStart = (& $cli status $fpid | ConvertFrom-Json).offscreen_render.reallocations
    foreach ($stage in @(
        @{ label='S1_stress0_idle';  stress=0;  wait=1400 },
        @{ label='S2_stress64_hit';  stress=64; wait=2200 },
        @{ label='S3_stress64_hold'; stress=64; wait=1800 },
        @{ label='S4_stress0_recov'; stress=0;  wait=2800 }
    )) {
        & $cli set $fpid stress $stage.stress | Out-Null
        Start-Sleep -Milliseconds $stage.wait
        $st = (& $cli status $fpid | ConvertFrom-Json)
        $tr = (& $cli trace $fpid 100 summary | ConvertFrom-Json)
        $s = $tr.summary
        $r = $st.offscreen_render
        Write-Host ("  {0,-18} stress={1,2}  scale={2,3}% ({3}x{4})  target={5,4}us  scn_med={6,5}us(p95={7,5})  misses={8}  new_reallocs={9}" -f $stage.label, $stage.stress, $r.scale_pct, $r.width, $r.height, $r.target_scene_budget_us, $s.scene_gpu_us.median, $s.scene_gpu_us.p95, $s.missed_deadlines, ($r.reallocations - $reallocStart))
    }

    Write-Host "`n=== EXP 3: STACKED AUTO PER-FRAME DRS + PAR 15% AT TRUE 4K BACKING (3840x2160, 120Hz, stress 0 -> 64 -> 0) ===" -ForegroundColor Cyan
    & $cli set $fpid drs off | Out-Null
    & $cli render_size $fpid 3840 2160 | Out-Null
    & $cli set $fpid selective_tiles on | Out-Null
    & $cli set $fpid tile_budget 15 | Out-Null
    & $cli set $fpid drs_min 50 | Out-Null
    & $cli set $fpid drs_max 100 | Out-Null
    & $cli set $fpid drs auto | Out-Null
    Start-Sleep -Milliseconds 300
    $realloc4k = (& $cli status $fpid | ConvertFrom-Json).offscreen_render.reallocations

    foreach ($stage in @(
        @{ label='4K_S1_stress0';   stress=0;  wait=1400 },
        @{ label='4K_S2_stress64';  stress=64; wait=2400 },
        @{ label='4K_S3_recov0';    stress=0;  wait=2800 }
    )) {
        & $cli set $fpid stress $stage.stress | Out-Null
        Start-Sleep -Milliseconds $stage.wait
        $st = (& $cli status $fpid | ConvertFrom-Json)
        $tr = (& $cli trace $fpid 100 summary | ConvertFrom-Json)
        $s = $tr.summary
        $r = $st.offscreen_render
        Write-Host ("  {0,-16} stress={1,2}  scale={2,3}% ({3}x{4} / alloc {5}x{6})  scn_med={7,5}us(p95={8,5})  misses={9}  new_reallocs={10}" -f $stage.label, $stage.stress, $r.scale_pct, $r.width, $r.height, $r.alloc_width, $r.alloc_height, $s.scene_gpu_us.median, $s.scene_gpu_us.p95, $s.missed_deadlines, ($r.reallocations - $realloc4k))
    }
} finally {
    & $cli stop $fpid | Out-Null
    Start-Sleep -Milliseconds 500
    foreach ($f in $tempPpms) { if ($f -and (Test-Path $f)) { Remove-Item $f -Force } }
}

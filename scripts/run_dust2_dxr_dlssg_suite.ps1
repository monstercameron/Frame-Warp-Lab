param(
    [string]$OutDir = "C:\Users\mreca\.t3\userdata\providers\antigravity\ac0a3dfd6dddb20962cecff6ee5fe65e19d3923be20e52c5ab52ff877f7e4c32\antigravity-acp\brain\392798e0-ab7f-42a1-a0be-9003b1bd0ee0\scratch"
)

Set-Location "C:\Users\mreca\Desktop\Frame Warp Lab"
New-Item -ItemType Directory -Force -Path $OutDir | Out-Null

Add-Type @"
using System;
using System.Runtime.InteropServices;
public static class WinFocusHelper2 {
    [StructLayout(LayoutKind.Sequential)] public struct RECT { public int Left, Top, Right, Bottom; }
    [DllImport("user32.dll")] public static extern IntPtr GetForegroundWindow();
    [DllImport("user32.dll")] public static extern uint GetWindowThreadProcessId(IntPtr hWnd, out uint lpdwProcessId);
    [DllImport("kernel32.dll")] public static extern uint GetCurrentThreadId();
    [DllImport("user32.dll")] public static extern bool AttachThreadInput(uint idAttach, uint idAttachTo, bool fAttach);
    [DllImport("user32.dll")] public static extern bool SetForegroundWindow(IntPtr hWnd);
    [DllImport("user32.dll")] public static extern bool BringWindowToTop(IntPtr hWnd);
    [DllImport("user32.dll")] public static extern bool ShowWindow(IntPtr hWnd, int nCmdShow);
    [DllImport("user32.dll")] public static extern bool GetWindowRect(IntPtr hWnd, out RECT lpRect);
    [DllImport("user32.dll")] public static extern bool SetCursorPos(int X, int Y);
    [DllImport("user32.dll")] public static extern void mouse_event(uint dwFlags, int dx, int dy, uint dwData, UIntPtr dwExtraInfo);
    [DllImport("user32.dll")] public static extern void keybd_event(byte bVk, byte bScan, uint dwFlags, UIntPtr dwExtraInfo);

    public static void ForceForeground(IntPtr hWnd) {
        if (hWnd == IntPtr.Zero) return;
        IntPtr fg = GetForegroundWindow();
        uint dummy;
        uint fgTid = fg != IntPtr.Zero ? GetWindowThreadProcessId(fg, out dummy) : 0;
        uint targetTid = GetWindowThreadProcessId(hWnd, out dummy);
        uint curTid = GetCurrentThreadId();
        keybd_event(0x12, 0, 0, UIntPtr.Zero);
        keybd_event(0x12, 0, 2, UIntPtr.Zero);
        if (fgTid != 0 && fgTid != curTid) AttachThreadInput(curTid, fgTid, true);
        if (targetTid != 0 && targetTid != curTid) AttachThreadInput(curTid, targetTid, true);
        ShowWindow(hWnd, 9);
        BringWindowToTop(hWnd);
        SetForegroundWindow(hWnd);
        if (targetTid != 0 && targetTid != curTid) AttachThreadInput(curTid, targetTid, false);
        if (fgTid != 0 && fgTid != curTid) AttachThreadInput(curTid, fgTid, false);
        RECT rc;
        if (GetWindowRect(hWnd, out rc)) {
            int cx = (rc.Left + rc.Right) / 2;
            int cy = rc.Top + 14;
            SetCursorPos(cx, cy);
            mouse_event(0x0002, 0, 0, 0, UIntPtr.Zero);
            mouse_event(0x0004, 0, 0, 0, UIntPtr.Zero);
        }
    }
}
"@

$cli = ".\build\Release\frame_warp_cli.exe"
Get-Process -Name frame_warp_lab -ErrorAction SilentlyContinue | Stop-Process -Force
Start-Sleep -Milliseconds 400

$startJson = (& $cli start | Out-String) | ConvertFrom-Json
$appPid = $startJson.process_id
Write-Output "Started Release PID: $appPid"

$ready = $false
for ($i = 0; $i -lt 120; $i++) {
    $st = (& $cli status $appPid 2>&1 | Out-String)
    if ($st -match '"ok":true') { $ready = $true; Write-Output "Ready after $($i*200)ms"; break }
    Start-Sleep -Milliseconds 200
}
if (-not $ready) { throw "App did not become ready" }

function Invoke-Cli($argsList) {
    return (& $cli @argsList 2>&1 | Out-String)
}

function Sample-Config($label, $warmupMs, $sampleMs, $injectInput = $false) {
    Start-Sleep -Milliseconds $warmupMs
    $st0 = (Invoke-Cli @("status", $appPid)) | ConvertFrom-Json
    $sw = [System.Diagnostics.Stopwatch]::StartNew()
    if ($injectInput) {
        $steps = [math]::Max(1, [int]($sampleMs / 60))
        for ($k = 0; $k -lt $steps; $k++) {
            $dx = if (($k % 2) -eq 0) { 1 } else { -1 }
            [WinFocusHelper2]::mouse_event(0x0001, $dx, 0, 0, [UIntPtr]::Zero)
            Start-Sleep -Milliseconds 60
        }
    } else {
        Start-Sleep -Milliseconds $sampleMs
    }
    $sw.Stop()
    $elapsedSec = $sw.Elapsed.TotalSeconds
    $tr = (Invoke-Cli @("trace", $appPid, "180", "summary")) | ConvertFrom-Json
    $st = (Invoke-Cli @("status", $appPid)) | ConvertFrom-Json
    $dPresent = $st.dlss_g.total_frames_presented - $st0.dlss_g.total_frames_presented
    $dGen = $st.dlss_g.total_generated_frames - $st0.dlss_g.total_generated_frames
    $effFps = if ($elapsedSec -gt 0.05 -and $dPresent -gt 0) { [math]::Round($dPresent / $elapsedSec, 1) } else { $st.timing.smoothed_present_fps }
    return [pscustomobject]@{
        label = $label
        render_w = $st.offscreen_render.width
        render_h = $st.offscreen_render.height
        scene_gpu_med_us = $tr.summary.scene_gpu_us.median
        scene_gpu_p95_us = $tr.summary.scene_gpu_us.p95
        present_gpu_med_us = $tr.summary.present_gpu_us.median
        present_interval_med_us = $tr.summary.cpu_present_interval_us.median
        input_to_present_med_us = if ($tr.summary.input_to_present_us) { $tr.summary.input_to_present_us.median } else { $null }
        input_to_present_p95_us = if ($tr.summary.input_to_present_us) { $tr.summary.input_to_present_us.p95 } else { $null }
        smoothed_fps = $st.timing.smoothed_present_fps
        dlssg_active = $st.dlss_g.active
        window_focused = $st.dlss_g.window_focused
        dlssg_mult = $st.dlss_g.requested_multiplier
        dlssg_last_presented = $st.dlss_g.last_frames_presented
        dlssg_delta_presented = $dPresent
        dlssg_delta_generated = $dGen
        dlssg_eff_fps = $effFps
        sc_hit_rate = $st.selective_tiles.surface_cache.hit_rate
        sc_collision_rate = $st.selective_tiles.surface_cache.collision_rate
        avg_lights_tile = $st.selective_tiles.surface_cache.avg_active_lights_per_tile
        shaded_tile_ratio = $st.selective_tiles.shaded_tile_ratio
        d2_sub_tris = $st.dust2.submitted_tris
        d2_tot_tris = $st.dust2.total_lod0_tris
        d2_culled = $st.dust2.octree_culled_nodes
        d2_lod0 = $st.dust2.lod0_chunks
        d2_lod1 = $st.dust2.lod1_chunks
        d2_lod2 = $st.dust2.lod2_chunks
        d2_probes = $st.dust2.active_probes
        d2_lights = $st.dust2.active_lights
        d2_pbr_ms = $st.dust2.pbr_build_ms
        dxr_tier = $st.dxr.tier
    }
}

$results = @()

Write-Output "=== PART 1: 720p FULL-PBR 512x512 + POM QUALITY & VISUAL CAPTURE ==="
Invoke-Cli @("set", $appPid, "scene", "dust2") | Out-Null
Invoke-Cli @("set", $appPid, "overlay", "off") | Out-Null
Invoke-Cli @("set", $appPid, "pacer", "unlocked") | Out-Null
Invoke-Cli @("set", $appPid, "vsync", "off") | Out-Null
Invoke-Cli @("set", $appPid, "clustered_lighting", "on") | Out-Null
Invoke-Cli @("set", $appPid, "surface_cache_mb", "16") | Out-Null
Invoke-Cli @("set", $appPid, "surface_cache", "on") | Out-Null
Invoke-Cli @("set", $appPid, "split_diff_spec", "on") | Out-Null
Invoke-Cli @("set", $appPid, "half_res_spec", "on") | Out-Null
Invoke-Cli @("set", $appPid, "fixed_dt_us", "16667") | Out-Null
Invoke-Cli @("pause", $appPid) | Out-Null
Start-Sleep -Milliseconds 300

function Capture-Pair($tagBase, $selTiles, $splitSpec, $halfSpec, $shaderFeat) {
    Invoke-Cli @("reset", $appPid) | Out-Null
    Start-Sleep -Milliseconds 120
    Invoke-Cli @("set", $appPid, "path", "corridor_sway") | Out-Null
    Invoke-Cli @("set", $appPid, "selective_tiles", $selTiles) | Out-Null
    Invoke-Cli @("set", $appPid, "tile_budget", "15") | Out-Null
    Invoke-Cli @("set", $appPid, "subtile_interleave", "on") | Out-Null
    Invoke-Cli @("set", $appPid, "split_diff_spec", $splitSpec) | Out-Null
    Invoke-Cli @("set", $appPid, "half_res_spec", $halfSpec) | Out-Null
    Invoke-Cli @("set", $appPid, "shader_feature", $shaderFeat) | Out-Null
    Start-Sleep -Milliseconds 180
    $c1 = (Invoke-Cli @("capture", $appPid, "$tagBase", "40")) | ConvertFrom-Json
    $c2 = (Invoke-Cli @("capture", $appPid, "$($tagBase + 1)", "1")) | ConvertFrom-Json
    Write-Host "Captured tag=$tagBase t=$($c1.scene_time_us)us & $($c2.scene_time_us)us"
    return @($c1.path, $c2.path)
}

$pGt = Capture-Pair 401 "off" "on" "off" "none"
$pParFull = Capture-Pair 411 "on" "on" "off" "none"
$pParHalf = Capture-Pair 421 "on" "on" "on" "none"
$pDxrGt = Capture-Pair 431 "off" "on" "off" "gi_bounce"
$pDxrPar = Capture-Pair 441 "on" "on" "on" "gi_bounce"

$pyScript = @"
import json, math
from PIL import Image

def load_rgb_bytes(p):
    im = Image.open(p).convert('RGB')
    return im.size, im.tobytes()

def metrics(p_ref, p_tst):
    sz, b_r = load_rgb_bytes(p_ref)
    _, b_t = load_rgb_bytes(p_tst)
    n_px = sz[0] * sz[1]
    mse_sum = 0.0
    sum_x = sum_y = sum_xx = sum_yy = sum_xy = 0.0
    for i in range(0, len(b_r), 3):
        r0, g0, b0 = b_r[i], b_r[i+1], b_r[i+2]
        r1, g1, b1 = b_t[i], b_t[i+1], b_t[i+2]
        dr = r0 - r1; dg = g0 - g1; db = b0 - b1
        mse_sum += dr*dr + dg*dg + db*db
        yx = 0.299*r0 + 0.587*g0 + 0.114*b0
        yy = 0.299*r1 + 0.587*g1 + 0.114*b1
        sum_x += yx; sum_y += yy
        sum_xx += yx*yx; sum_yy += yy*yy; sum_xy += yx*yy
    mse = mse_sum / (n_px * 3.0)
    psnr = 99.0 if mse < 1e-9 else 10.0 * math.log10((255.0*255.0) / mse)
    mx = sum_x / n_px; my = sum_y / n_px
    vx = max(0.0, sum_xx / n_px - mx*mx)
    vy = max(0.0, sum_yy / n_px - my*my)
    cov = sum_xy / n_px - mx*my
    c1, c2 = (0.01*255)**2, (0.03*255)**2
    ssim = ((2*mx*my + c1)*(2*cov + c2)) / ((mx*mx + my*my + c1)*(vx + vy + c2))
    return round(psnr, 2), round(ssim, 4)

def trmse(p_r0, p_r1, p_t0, p_t1):
    _, r0 = load_rgb_bytes(p_r0)
    _, r1 = load_rgb_bytes(p_r1)
    _, t0 = load_rgb_bytes(p_t0)
    _, t1 = load_rgb_bytes(p_t1)
    s = 0.0
    for i in range(len(r0)):
        d = (t1[i] - t0[i]) - (r1[i] - r0[i])
        s += d*d
    return round(math.sqrt(s / len(r0)), 2)

Image.open(r'$($pGt[0])').save(r'$OutDir\dust2_pbr512_gt.png')
Image.open(r'$($pDxrGt[0])').save(r'$OutDir\dust2_pbr512_dxr_gt.png')
Image.open(r'$($pDxrPar[0])').save(r'$OutDir\dust2_pbr512_dxr_par.png')

out = {
    'dust2_pbr512_par_split_fullspec_vs_gt': {'psnr': metrics(r'$($pGt[0])', r'$($pParFull[0])')[0], 'ssim': metrics(r'$($pGt[0])', r'$($pParFull[0])')[1], 'trmse': trmse(r'$($pGt[0])', r'$($pGt[1])', r'$($pParFull[0])', r'$($pParFull[1])')},
    'dust2_pbr512_par_split_halfspec_vs_gt': {'psnr': metrics(r'$($pGt[0])', r'$($pParHalf[0])')[0], 'ssim': metrics(r'$($pGt[0])', r'$($pParHalf[0])')[1], 'trmse': trmse(r'$($pGt[0])', r'$($pGt[1])', r'$($pParHalf[0])', r'$($pParHalf[1])')},
    'dust2_pbr512_dxr_rtgi_par_vs_dxr_gt': {'psnr': metrics(r'$($pDxrGt[0])', r'$($pDxrPar[0])')[0], 'ssim': metrics(r'$($pDxrGt[0])', r'$($pDxrPar[0])')[1], 'trmse': trmse(r'$($pDxrGt[0])', r'$($pDxrGt[1])', r'$($pDxrPar[0])', r'$($pDxrPar[1])')},
    'dust2_pbr512_dxr_vs_probes_delta': {'psnr': metrics(r'$($pGt[0])', r'$($pDxrGt[0])')[0], 'ssim': metrics(r'$($pGt[0])', r'$($pDxrGt[0])')[1]}
}
print(json.dumps(out, indent=2))
"@
$qualJson = python -c $pyScript
Write-Output "QUALITY_METRICS: $qualJson"
$qualJson | Set-Content (Join-Path $OutDir "dust2_pbr512_quality.json")

Write-Output "=== PART 2: PRIORITY 10 — 1440p & 4K 512x512 PBR+POM + 64 LIGHTS + HALF-RES SPEC ==="
Invoke-Cli @("set", $appPid, "fixed_dt_us", "0") | Out-Null
Invoke-Cli @("resume", $appPid) | Out-Null
Invoke-Cli @("set", $appPid, "path", "corridor_sway") | Out-Null
Invoke-Cli @("set", $appPid, "shader_feature", "none") | Out-Null
Invoke-Cli @("set", $appPid, "mode", "full_render") | Out-Null
Invoke-Cli @("set", $appPid, "clustered_lighting", "on") | Out-Null
Invoke-Cli @("set", $appPid, "surface_cache_mb", "16") | Out-Null
Invoke-Cli @("set", $appPid, "surface_cache", "on") | Out-Null

foreach ($res in @(@(2560, 1440, "1440p"), @(3840, 2160, "4K"))) {
    $rw = $res[0]; $rh = $res[1]; $rname = $res[2]
    Invoke-Cli @("render_size", $appPid, "$rw", "$rh") | Out-Null
    Start-Sleep -Milliseconds 350

    Invoke-Cli @("set", $appPid, "selective_tiles", "off") | Out-Null
    $results += Sample-Config "Dust2_PBR512_${rname}_ForwardGT_64Lights" 500 1200

    Invoke-Cli @("set", $appPid, "selective_tiles", "on") | Out-Null
    Invoke-Cli @("set", $appPid, "tile_budget", "15") | Out-Null
    Invoke-Cli @("set", $appPid, "subtile_interleave", "on") | Out-Null
    Invoke-Cli @("set", $appPid, "split_diff_spec", "off") | Out-Null
    Invoke-Cli @("set", $appPid, "half_res_spec", "off") | Out-Null
    $results += Sample-Config "Dust2_PBR512_${rname}_PAR3_Unified_SC16" 500 1200

    Invoke-Cli @("set", $appPid, "split_diff_spec", "on") | Out-Null
    Invoke-Cli @("set", $appPid, "half_res_spec", "off") | Out-Null
    $results += Sample-Config "Dust2_PBR512_${rname}_PAR3_Split_FullSpec" 500 1200

    Invoke-Cli @("set", $appPid, "split_diff_spec", "on") | Out-Null
    Invoke-Cli @("set", $appPid, "half_res_spec", "on") | Out-Null
    $results += Sample-Config "Dust2_PBR512_${rname}_PAR3_Split_HalfResSpec" 500 1200
}

Write-Output "=== PART 3: HARDWARE DXR TIER 1.1 16-RAY RTGI SCALING (1080p / 1440p / 4K) ==="
Invoke-Cli @("set", $appPid, "shader_feature", "gi_bounce") | Out-Null
foreach ($res in @(@(1920, 1080, "1080p"), @(2560, 1440, "1440p"), @(3840, 2160, "4K"))) {
    $rw = $res[0]; $rh = $res[1]; $rname = $res[2]
    Invoke-Cli @("render_size", $appPid, "$rw", "$rh") | Out-Null
    Start-Sleep -Milliseconds 350

    Invoke-Cli @("set", $appPid, "selective_tiles", "off") | Out-Null
    $results += Sample-Config "Dust2_PBR512_${rname}_DXR_RTGI_ForwardGT" 600 1200

    Invoke-Cli @("set", $appPid, "selective_tiles", "on") | Out-Null
    Invoke-Cli @("set", $appPid, "tile_budget", "15") | Out-Null
    Invoke-Cli @("set", $appPid, "subtile_interleave", "on") | Out-Null
    Invoke-Cli @("set", $appPid, "split_diff_spec", "off") | Out-Null
    Invoke-Cli @("set", $appPid, "half_res_spec", "off") | Out-Null
    $results += Sample-Config "Dust2_PBR512_${rname}_DXR_RTGI_PAR3_Unified_SC16" 600 1200

    Invoke-Cli @("set", $appPid, "split_diff_spec", "on") | Out-Null
    Invoke-Cli @("set", $appPid, "half_res_spec", "on") | Out-Null
    $results += Sample-Config "Dust2_PBR512_${rname}_DXR_RTGI_PAR3_Split_HalfResSpec_SC16" 600 1200
}

Write-Output "=== PART 4: PRIORITY 8 — TRUE FOREGROUND DLSS-G MODE 6 + CLEAN-FEED PAR + 16-RAY DXR RTGI ==="
Invoke-Cli @("set", $appPid, "overlay", "off") | Out-Null
Invoke-Cli @("set", $appPid, "vsync", "off") | Out-Null
Invoke-Cli @("set", $appPid, "pacer", "unlocked") | Out-Null
Invoke-Cli @("set", $appPid, "selective_tiles", "on") | Out-Null
Invoke-Cli @("set", $appPid, "tile_budget", "15") | Out-Null
Invoke-Cli @("set", $appPid, "subtile_interleave", "on") | Out-Null
Invoke-Cli @("set", $appPid, "split_diff_spec", "on") | Out-Null
Invoke-Cli @("set", $appPid, "half_res_spec", "on") | Out-Null
Invoke-Cli @("set", $appPid, "surface_cache", "on") | Out-Null
Invoke-Cli @("set", $appPid, "surface_cache_mb", "16") | Out-Null
Invoke-Cli @("set", $appPid, "clustered_lighting", "on") | Out-Null
Invoke-Cli @("set", $appPid, "shader_feature", "gi_bounce") | Out-Null

$proc = Get-Process -Id $appPid
foreach ($res in @(@(2560, 1440, "1440p"), @(3840, 2160, "4K"))) {
    $rw = $res[0]; $rh = $res[1]; $rname = $res[2]
    Invoke-Cli @("render_size", $appPid, "$rw", "$rh") | Out-Null
    Start-Sleep -Milliseconds 350

    foreach ($mult in @(2, 4, 6)) {
        Invoke-Cli @("set", $appPid, "mode", "dlss_g") | Out-Null
        Invoke-Cli @("set", $appPid, "dlssg_mult", "$mult") | Out-Null
        Start-Sleep -Milliseconds 250
        $proc.Refresh()
        if ($proc.MainWindowHandle -ne [IntPtr]::Zero) {
            [WinFocusHelper2]::ForceForeground($proc.MainWindowHandle)
        }
        $results += Sample-Config "Dust2_PBR512_${rname}_FG_DLSSG_${mult}x_PAR3_DXR_RTGI" 500 1800 $true
    }
}

$results | ConvertTo-Json -Depth 4 | Set-Content (Join-Path $OutDir "dust2_pbr512_results.json")
$results | Format-Table -AutoSize label, scene_gpu_med_us, present_gpu_med_us, window_focused, dlssg_active, dlssg_last_presented, dlssg_delta_generated, dlssg_eff_fps, input_to_present_med_us

Invoke-Cli @("stop", $appPid) | Out-Null
Start-Sleep -Milliseconds 600
Get-Process -Id $appPid -ErrorAction SilentlyContinue | Stop-Process -Force
Write-Output "PBR512_SUITE_DONE"

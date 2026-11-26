param(
    [string]$Config = "Release",
    [int]$StepSeconds = 2,
    [int]$Stress = 24
)

$ErrorActionPreference = "Stop"
$RepoRoot = Split-Path -Parent $PSScriptRoot
$Cli = Join-Path $RepoRoot "build\$Config\frame_warp_cli.exe"
if (-not (Test-Path $Cli)) {
    throw "Could not find $Cli. Build the project first with: cmake --build build --config $Config"
}

Write-Host "Querying live Streamline / DLSS-G capabilities..."
$capLine = & $Cli capabilities | Select-Object -Last 1
$caps = $capLine | ConvertFrom-Json
Write-Host ("GPU: {0} | SL {1} | NGX {2} | DLSS-G supported={3} | Max={4}x | DynamicMFG={5}" -f `
    $caps.adapter.description,
    $caps.dlss_frame_generation.sl_version,
    $caps.dlss_frame_generation.ngx_version,
    $caps.dlss_frame_generation.supported,
    $caps.dlss_frame_generation.max_multiplier,
    $caps.dlss_frame_generation.dynamic_mfg_supported)

$startOut = (& $Cli start | Select-Object -Last 1) | ConvertFrom-Json
$procId = $startOut.process_id
Write-Host "Started frame_warp_lab.exe (PID $procId). Keep its window focused during the test..."

$ws = New-Object -ComObject WScript.Shell
for ($i = 0; $i -lt 40; $i++) {
    Start-Sleep -Milliseconds 250
    $p = Get-Process -Id $procId -ErrorAction SilentlyContinue
    if ($p -and $p.MainWindowHandle -ne [IntPtr]::Zero) {
        $ws.AppActivate($procId) | Out-Null
        break
    }
}
Start-Sleep -Milliseconds 600

function Measure-Stage($Label) {
    Start-Sleep -Seconds $StepSeconds
    $st = (& $Cli status $procId | Select-Object -Last 1) | ConvertFrom-Json
    $tr = (& $Cli trace $procId 120 summary | Select-Object -Last 1) | ConvertFrom-Json
    $avgPresented = if ($tr.sample_count -gt 0) { [math]::Round($tr.summary.dlssg_frames_presented / $tr.sample_count, 2) } else { 0.0 }
    $effFps = [math]::Round($st.timing.smoothed_present_fps * [math]::Max(1.0, $avgPresented), 1)
    Write-Host ("[{0}] mode={1} owner={2} focused={3} mult={4} last_pres={5} avg_pres={6}x total_gen={7} app_fps={8} est_out_fps={9} scene={10}us pres={11}us status={12} api_err={13} guard_trips={14}" -f `
        $Label,
        $st.presentation.mode,
        $st.presentation.pacing_owner,
        $st.dlss_g.window_focused,
        $st.dlss_g.requested_multiplier,
        $st.dlss_g.last_frames_presented,
        $avgPresented,
        $st.dlss_g.total_generated_frames,
        $st.timing.smoothed_present_fps,
        $effFps,
        $tr.summary.scene_gpu_us.median,
        $tr.summary.present_gpu_us.median,
        $st.dlss_g.status_flags,
        $st.dlss_g.api_error_count,
        $st.dlss_g.warp_guard_trips)
}

try {
    & $Cli set $procId stress $Stress | Out-Null
    & $Cli set $procId pacer unlocked | Out-Null
    & $Cli set $procId vsync off | Out-Null

    & $Cli set $procId mode dlss_g | Out-Null
    & $Cli set $procId dlssg_mult 2 | Out-Null
    Measure-Stage "DLSS-G 2x"

    & $Cli set $procId dlssg_mult 3 | Out-Null
    Measure-Stage "DLSS-G 3x"

    & $Cli set $procId dlssg_mult 4 | Out-Null
    Measure-Stage "DLSS-G 4x"

    if ($caps.dlss_frame_generation.max_multiplier -ge 6) {
        & $Cli set $procId dlssg_mult 6 | Out-Null
        Measure-Stage "DLSS-G 6x"
    }

    if ($caps.dlss_frame_generation.dynamic_mfg_supported) {
        & $Cli set $procId dlssg_mult dynamic | Out-Null
        & $Cli set $procId target_fps 240 | Out-Null
        Measure-Stage "DLSS-G Dynamic 240"
        & $Cli set $procId target_fps 0 | Out-Null
    }

    & $Cli set $procId mode dlss_g_warp | Out-Null
    & $Cli set $procId path occlusion_sweep | Out-Null
    & $Cli set $procId divider 2 | Out-Null
    & $Cli set $procId dlssg_mult 2 | Out-Null
    Measure-Stage "SpaceWarp + DLSS-G 2x"

    & $Cli set $procId dlssg_mult 4 | Out-Null
    & $Cli set $procId vrs foveated | Out-Null
    Measure-Stage "SpaceWarp + DLSS-G 4x + VRS"

    & $Cli set $procId pacer deadline | Out-Null
    & $Cli set $procId target_fps 360 | Out-Null
    & $Cli set $procId stress 64 | Out-Null
    & $Cli set $procId adaptive_dlssg on | Out-Null
    & $Cli set $procId adaptive on | Out-Null
    Measure-Stage "Unified Adaptive 360Hz"
}
finally {
    & $Cli stop $procId | Out-Null
    Start-Sleep -Milliseconds 600
}

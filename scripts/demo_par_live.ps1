param(
    [int]$SecondsPerStage = 5,
    [switch]$KeepOpen = $true
)

$ErrorActionPreference = 'Stop'
$repoRoot = Split-Path -Parent $PSScriptRoot
$exe = Join-Path $repoRoot 'build\Release\frame_warp_lab.exe'
$cli = Join-Path $repoRoot 'build\Release\frame_warp_cli.exe'

if (-not (Test-Path $exe) -or -not (Test-Path $cli)) {
    throw "Release binaries not found. Build first: cmake --build build --config Release"
}

$proc = Start-Process -FilePath $exe -PassThru
$fpid = $proc.Id
Write-Host "Launched Frame Warp Lab (PID $fpid) on your desktop..." -ForegroundColor Cyan

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

$stages = @(
    @{
        title = '1/6  Whole-Frame Native Baseline (stress=64, 100% pixels shaded)'
        cmds  = @(
            @('target_fps', '120'), @('stress', '64'), @('path', 'occlusion_sweep'),
            @('mode', 'full_render'), @('divider', '1'), @('vrs', 'off'),
            @('selective_tiles', 'off'), @('view', 'final'), @('overlay', 'on')
        )
    },
    @{
        title = '2/6  Hardware VRS Tier 2 Foveated (stress=64)'
        cmds  = @(
            @('vrs', 'foveated'), @('selective_tiles', 'off'), @('view', 'final')
        )
    },
    @{
        title = '3/6  PAR 15% Priority + STBN + Catmull-Rom/TRM (full 1x1 quality, ~5-7x faster)'
        cmds  = @(
            @('vrs', 'off'), @('selective_tiles', 'on'), @('tile_policy', 'priority_stbn'),
            @('tile_budget', '15'), @('cache_sharpen', 'on'), @('view', 'final')
        )
    },
    @{
        title = '4/6  PAR Live Perceptual Priority Heatmap (view=priority: red=critical, blue=reusable)'
        cmds  = @(
            @('view', 'priority')
        )
    },
    @{
        title = '5/6  PAR Live 16x16 Tile Map (view=tiles: green=cached, orange=dirty, cyan=STBN, magenta=deferred)'
        cmds  = @(
            @('view', 'tiles')
        )
    },
    @{
        title = '6/6  Clean-Feed PAR 15% + NVIDIA DLSS-G 2x (keep window focused to see real DLSS-G)'
        cmds  = @(
            @('view', 'final'), @('mode', 'dlss_g_warp'), @('divider', '1'),
            @('dlssg_mult', '2'), @('async_gpu', 'off'), @('tile_budget', '15')
        )
    }
)

foreach ($s in $stages) {
    Write-Host "`n>>> $($s.title)" -ForegroundColor Yellow
    foreach ($c in $s.cmds) {
        & $cli set $fpid $c[0] $c[1] | Out-Null
    }
    Start-Sleep -Seconds $SecondsPerStage
    $st = (& $cli status $fpid | ConvertFrom-Json)
    $crit = if ($null -ne $st.selective_tiles.critical_coverage_ratio) { "{0:N1}%" -f ($st.selective_tiles.critical_coverage_ratio * 100) } else { "n/a" }
    Write-Host ("    FPS={0:N1}  scene_gpu={1}us  present_gpu={2}us  tiles={3}/{4}  crit_cov={5}  dlssg_active={6}  dlssg_presented={7}  focused={8}" -f $st.timing.smoothed_present_fps, $st.timing.scene_gpu_us, $st.timing.present_gpu_us, $st.selective_tiles.shaded_tiles, $st.selective_tiles.total_tiles, $crit, $st.dlss_g.active, $st.dlss_g.last_frames_presented, $st.dlss_g.window_focused) -ForegroundColor Green
}

if (-not $KeepOpen) {
    & $cli stop $fpid | Out-Null
} else {
    Write-Host "`nDemo complete! Window left open (PID $fpid). Hotkeys: U=PAR tiles, Y=priority heatmap, T=tile map, 1-7=modes, X=DLSS mult, C=paths, M/RMB=mouse look." -ForegroundColor Cyan
}

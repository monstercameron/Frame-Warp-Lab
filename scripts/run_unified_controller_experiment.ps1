$ErrorActionPreference = 'Stop'
$repoRoot = Split-Path -Parent $PSScriptRoot
$cli = Join-Path $repoRoot 'build\Release\frame_warp_cli.exe'

$fpid = (& $cli start | ConvertFrom-Json).process_id
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
    & $cli set $fpid pacer deadline | Out-Null
    & $cli set $fpid target_fps 120 | Out-Null
    & $cli set $fpid async_gpu on | Out-Null
    & $cli set $fpid path corridor_sway | Out-Null

    Write-Host "`n=== PRIORITY 7: UNIFIED COST & QUALITY CONTROLLER vs INDEPENDENT CONFLICTING CONTROLLERS ===" -ForegroundColor Cyan
    Write-Host "    4-Stage Resolution Load Shock @ 120Hz Paced (14-Light PBR Gun Range): 1080p -> 4K -> 1440p -> 4K" -ForegroundColor Gray

    $stages = @(
        @{ name='S1_1080p'; w=1920; h=1080; ms=700 },
        @{ name='S2_4K';    w=3840; h=2160; ms=900 },
        @{ name='S3_1440p'; w=2560; h=1440; ms=800 },
        @{ name='S4_4K';    w=3840; h=2160; ms=900 }
    )

    foreach ($mode in @('independent', 'unified')) {
        & $cli set $fpid unified_controller off | Out-Null
        & $cli set $fpid adaptive off | Out-Null
        & $cli set $fpid auto_tile_budget off | Out-Null
        & $cli set $fpid drs off | Out-Null
        & $cli set $fpid render_scale 100 | Out-Null
        & $cli set $fpid vrs off | Out-Null
        & $cli set $fpid mode full_render | Out-Null
        & $cli set $fpid divider 1 | Out-Null
        & $cli set $fpid selective_tiles on | Out-Null
        & $cli set $fpid subtile_interleave on | Out-Null
        & $cli set $fpid split_diff_spec on | Out-Null
        & $cli set $fpid surface_cache on | Out-Null
        & $cli set $fpid tile_budget 25 | Out-Null
        & $cli render_size $fpid 1920 1080 | Out-Null
        Start-Sleep -Milliseconds 300

        $stStart = (& $cli status $fpid | ConvertFrom-Json)
        $missStart = $stStart.presentation.missed_deadlines
        $drsChangesStart = $stStart.offscreen_render.scale_changes
        $adaptDecStart = $stStart.adaptive_scheduler.decision_count

        if ($mode -eq 'independent') {
            & $cli set $fpid adaptive on | Out-Null
            & $cli set $fpid auto_tile_budget on | Out-Null
            & $cli set $fpid drs auto | Out-Null
            Write-Host "`n--- Control Mode: INDEPENDENT CONTROLLERS (Adaptive VRS/Cadence + Auto PAR + Auto DRS competing) ---" -ForegroundColor Yellow
        } else {
            & $cli set $fpid unified_controller on | Out-Null
            Write-Host "`n--- Control Mode: PRIORITY 7 UNIFIED COST & QUALITY CONTROLLER (Single 7-Tier Pareto Ladder) ---" -ForegroundColor Green
        }

        $prevScale = 100
        $prevBud = 25
        $dirChanges = 0
        $lastScaleDir = 0
        $p95Sum = 0.0
        $medSum = 0.0
        foreach ($st in $stages) {
            & $cli render_size $fpid $st.w $st.h | Out-Null
            $polls = [int]($st.ms / 150)
            for ($k = 0; $k -lt $polls; $k++) {
                Start-Sleep -Milliseconds 150
                $cur = (& $cli status $fpid | ConvertFrom-Json)
                $sc = $cur.offscreen_render.scale_pct
                if ($sc -ne $prevScale) {
                    $d = if ($sc -gt $prevScale) { 1 } else { -1 }
                    if ($lastScaleDir -ne 0 -and $d -ne $lastScaleDir) { $dirChanges++ }
                    $lastScaleDir = $d
                    $prevScale = $sc
                }
            }
            $tr = (& $cli trace $fpid 45 | ConvertFrom-Json)
            $cur = (& $cli status $fpid | ConvertFrom-Json)
            $med = $tr.summary.scene_gpu_us.median
            $p95 = $tr.summary.scene_gpu_us.p95
            $medSum += $med; $p95Sum += $p95
            Write-Host ("  {0,-9} | med={1,4}us  p95={2,4}us  miss={3}  tier={4}  split={5,-5}  bud={6,3}%  scale={7,3}%  vrs={8,-8}  mode={9}/div{10}" -f `
                $st.name, $med, $p95, $tr.summary.missed_deadlines, `
                $(if ($mode -eq 'unified') { $cur.unified_controller.tier } else { $cur.adaptive_scheduler.tier }), `
                $cur.selective_tiles.split_diff_spec_enabled, $cur.selective_tiles.budget_pct, $cur.offscreen_render.scale_pct, `
                $cur.vrs.active_mode, $cur.presentation.mode, $cur.presentation.source_divider)
        }

        $stEnd = (& $cli status $fpid | ConvertFrom-Json)
        $totalMiss = $stEnd.presentation.missed_deadlines - $missStart
        $totalDrsChanges = $stEnd.offscreen_render.scale_changes - $drsChangesStart
        $totalTransitions = if ($mode -eq 'unified') { $stEnd.unified_controller.transitions } else { ($stEnd.adaptive_scheduler.decision_count - $adaptDecStart) + $totalDrsChanges }
        $osc = if ($mode -eq 'unified') { $stEnd.unified_controller.oscillations } else { $dirChanges }
        Write-Host ("  SUMMARY ({0}): avg_med={1}us  avg_p95={2}us  missed_deadlines={3}  actuator_changes={4}  oscillations={5}" -f `
            $mode, [math]::Round($medSum / 4.0, 0), [math]::Round($p95Sum / 4.0, 0), $totalMiss, $totalTransitions, $osc) -ForegroundColor Cyan
    }
} finally {
    & $cli stop $fpid | Out-Null
    Start-Sleep -Milliseconds 400
}

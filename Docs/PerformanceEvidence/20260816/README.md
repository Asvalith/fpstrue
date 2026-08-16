# 2026-08-16 Performance Evidence

## Fixed Enemy Matrix

| Screenshot | Purpose |
| --- | --- |
| `FinalLOD_10.png` | 10-enemy reference frame |
| `FinalLOD_160.png` | 160-enemy stress frame |

The complete five-level matrix, including CSV, logs, screenshots, manifest and summary, is stored locally at:

```text
Saved/Profiling/FPS_FinalLOD_20260816
```

## VSM Coarse Pages A/B

| Variant | FPS | Frame | Render Thread | VSM Queue Overflow |
| --- | ---: | ---: | ---: | ---: |
| Default (`IncludeInCoarsePages=1`) | 55.19 | 18.121 ms | 17.378 ms | 1 |
| Disabled (`IncludeInCoarsePages=0`) | 45.79 | 21.838 ms | 20.690 ms | 0 |

Screenshots:

```text
VSM_CoarsePagesOn_80.png
VSM_CoarsePagesOff_80.png
```

Disabling coarse pages removed the warning in this single run but caused a clear frame-time and Render Thread regression. The project therefore keeps the engine default. Other 80-enemy variants each logged one overflow, so the warning is documented as a single-event but reproducible residual risk, not as fully eliminated.

Detailed data and the additional dynamic-threshold and radius-threshold experiments are stored at:

```text
Saved/Profiling/VSM_CoarsePages_AB_20260816
Saved/Profiling/VSM_DynamicThreshold_20260816
Saved/Profiling/VSM_RadiusThreshold_20260816
```

## Lifecycle Cleanup

The 80-enemy lifecycle test used the normal damage and death chain, waited 35 seconds for the 30-second lifespan, and then captured a second profile.

| State | Enemy Actors | FPS | Game Thread | Movement | Animation | UObjects |
| --- | ---: | ---: | ---: | ---: | ---: | ---: |
| Alive | 80 | 44.96 | 18.110 ms | 3.705 ms | 2.915 ms | 50,763 |
| Post cleanup | 0 | 68.51 | 5.444 ms | 0.106 ms | 0.266 ms | 49,876 |

Files:

```text
Lifecycle_Alive_80.png
Lifecycle_PostCleanup_80.png
Lifecycle_80_summary.csv
```

The process working set rose from 3616.00 MB to 3676.34 MB because allocator retention and report timing affect physical memory. Actor, registration and UObject counts are used as the cleanup proof; the test does not claim an immediate process-memory reduction.

The temporary lifecycle trigger was removed after capture. The regular Development Editor target was rebuilt successfully; the local build log is `Saved/Logs/FinalPerformanceClosure_Build.log`.

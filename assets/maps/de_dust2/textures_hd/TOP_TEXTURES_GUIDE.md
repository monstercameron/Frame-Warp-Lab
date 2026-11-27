# de_dust2 HD Texture Override Guide (`textures_hd/`)

Drop any high-resolution PNG/JPG texture upscaled with the **ChatGPT Desktop app** (or any image generator) into this folder using the **exact filename stem** below (e.g. `material_36_49_baseColor.png`).

The engine automatically checks `assets/maps/de_dust2/textures_hd/` first on startup, loads your HD texture (at `512×512`–`1024×1024`), and generates matching 10-mip **Toksvig PBR normals, cavity AO, roughness, metallic, clearcoat, and 4-step Parallax Occlusion height maps**.

## Top 20 Highest-Impact Textures (Covers ~87.2% of `de_dust2`)

| Priority | Filename | Map Coverage | Original Size |
|---:|---|---:|---:|
| 1 | `material_36_49_baseColor.png` | 25.4% | `128×128` |
| 2 | `material_36_67_baseColor.png` | 9.9% | `64×32` |
| 3 | `material_36_76_baseColor.png` | 8.1% | `512×256` |
| 4 | `material_36_48_baseColor.png` | 7.4% | `256×128` |
| 5 | `material_36_91_baseColor.png` | 6.0% | `128×128` |
| 6 | `material_36_42_baseColor.png` | 3.7% | `32×128` |
| 7 | `material_36_77_baseColor.png` | 3.5% | *(missing in original)* |
| 8 | `material_36_69_baseColor.png` | 2.5% | `256×32` |
| 9 | `material_36_68_baseColor.png` | 2.2% | `32×64` |
| 10 | `material_36_92_baseColor.png` | 2.0% | `256×256` |
| 11 | `material_36_44_baseColor.png` | 1.9% | `128×128` |
| 12 | `material_36_21_baseColor.png` | 1.9% | `512×512` |
| 13 | `material_36_94_baseColor.png` | 1.8% | `128×128` |
| 14 | `material_36_90_baseColor.png` | 1.7% | `32×128` |
| 15 | `Merged_materials_baseColor.png` | 1.6% | `128×128` |
| 16 | `material_36_70_baseColor.png` | 1.5% | `256×256` |
| 17 | `material_36_16_baseColor.png` | 1.4% | `256×256` |
| 18 | `material_36_65_baseColor.png` | 1.3% | `64×64` |
| 19 | `material_36_19_baseColor.png` | 1.3% | `256×256` |
| 20 | `material_36_23_baseColor.png` | 1.1% | `128×128` |

## Recommended ChatGPT Desktop Prompt
Upload any file from `assets/maps/de_dust2/textures/` into the ChatGPT desktop app and use:

> *"Upscale and remaster this game texture into a crisp, photorealistic, seamlessly tileable 1024x1024 albedo diffuse texture. Keep the exact layout, color palette, proportions, and alignment so UV mapping stays identical. Flat even diffuse lighting with no baked directional shadows or perspective distortion."*

Save the result with the exact same filename inside `assets/maps/de_dust2/textures_hd/` and relaunch `frame_warp_lab.exe`.

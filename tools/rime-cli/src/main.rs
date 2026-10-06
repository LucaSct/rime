// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 The Rime Engine Authors.
//
// The Rime command-line tool. As of Milestone 6 it drives the offline asset pipeline: `cook`
// imports source assets (glTF or binary STL meshes, PNG/JPEG textures) and writes the engine's
// runtime RMA1 files; `inspect` prints
// a cooked file's header. Invoked with no subcommand it keeps the Milestone-0 stub banner. The CLI
// reaches the engine only across stable boundaries (cooked files here); see docs/adr/0001.

use std::path::{Path, PathBuf};
use std::process::ExitCode;

use asset_pipeline::cooked;
use asset_pipeline::texture::ColorSpace;
use clap::{Parser, Subcommand};

/// The tool's banner line, e.g. `"rime-cli 0.0.1"`. Kept as a small pure function so the version
/// lives in exactly one place — Cargo's `CARGO_PKG_VERSION` — and so it stays trivially testable.
fn banner() -> String {
    format!("rime-cli {}", env!("CARGO_PKG_VERSION"))
}

#[derive(Parser)]
#[command(name = "rime", version, about = "Rime engine command-line tools")]
struct Cli {
    #[command(subcommand)]
    command: Option<Command>,
}

#[derive(Subcommand)]
enum Command {
    /// Cook source assets (glTF/STL meshes, PNG/JPEG textures) into Rime's runtime RMA1 format.
    Cook {
        /// A `.gltf`/`.glb`/`.stl`/`.png`/`.jpg` file, or a directory of them.
        input: PathBuf,
        /// Output directory for the cooked files and `manifest.txt`.
        #[arg(long)]
        out: PathBuf,
        /// Treat texture inputs as sRGB colour (baseColor/emissive). The default.
        #[arg(long, conflicts_with = "linear")]
        srgb: bool,
        /// Treat texture inputs as linear data (normal / metallic-roughness / occlusion maps).
        #[arg(long)]
        linear: bool,
        /// Block-compress textures as BC7 (m16.7): a quarter the memory AND bandwidth, sampled
        /// directly by the GPU. Lossy — see `bcn.rs` for what the mode-6 encoder does and does not
        /// do. The cook cache keys on this, so flipping it re-cooks rather than serving the
        /// uncompressed bytes back.
        #[arg(long)]
        bc: bool,
    },
    /// Fracture a source box into a Destructible (M8.1): a wall/column/slab pre-split into convex
    /// parts with a bond/anchor graph, for the destruction runtime.
    Fracture {
        /// Full dimensions of the source box, in metres (X Y Z).
        #[arg(long, num_args = 3, value_names = ["X", "Y", "Z"])]
        size: Vec<f32>,
        /// Target number of parts (Voronoi cells).
        #[arg(long)]
        parts: u32,
        /// PRNG seed — the same seed + size + parts always cooks the identical partition.
        #[arg(long, default_value_t = 1)]
        seed: u64,
        /// Output directory for the `<name>.rdest` file.
        #[arg(long)]
        out: PathBuf,
        /// Output file stem (writes `<name>.rdest`).
        #[arg(long, default_value = "wall")]
        name: String,
        /// Impulse (kg·m/s) a part absorbs from a CONTACT before it takes any damage. Fences the
        /// resting case (a standing wall's own supports exchange m·g·dt every tick) — and, at the
        /// other end, decides whether falling rubble can destroy what it lands on.
        ///
        /// IT IS A PROPERTY OF THE PART'S MASS, not of the engine. The 5.0 default was tuned for
        /// M8's small test wall; an 8x3x0.3 m building slab in 12 pieces is two orders of magnitude
        /// heavier, and at 5.0 every debris impact kills the part it hits outright — which is what
        /// made one demolition charge flatten an entire city block (m13.5). Explicit `apply_damage`
        /// (a weapon, a charge) does not go through this, so raising it does not make a wall
        /// bulletproof.
        #[arg(long, default_value_t = 5.0)]
        damage_threshold: f32,
        /// Damage per unit of contact impulse above the threshold. Parts stand at 1.0 health, so
        /// `1 / (impulse you want to be lethal - threshold)` is the number to reason with.
        #[arg(long, default_value_t = 1.0)]
        damage_scale: f32,
    },
    /// Generate and cook the GROUND's material (m17.8b, ADR-0041 Ruling 5): a tiling asphalt
    /// albedo / normal / metallic-roughness set, plus the standalone material that references them.
    ///
    /// There is no input file. The generator is the source, exactly as for `fracture` — this
    /// repository has no texture art at all, and Ruling 5 wants the largest surface in the frame to
    /// stop being a constant colour that makes SSR, DDGI and shadows unjudgeable.
    Ground {
        /// Output directory for the cooked files and `manifest.txt`.
        #[arg(long)]
        out: PathBuf,
        /// Output file stem: writes `<name>.rmat` and `<name>_{albedo,normal,mr}.rtex`, labelled
        /// `ground:<name>#material0` and `ground:<name>#<map>` in the manifest.
        #[arg(long, default_value = "street")]
        name: String,
        /// Block-compress the maps as BC7 (m16.7): a quarter the memory and bandwidth.
        #[arg(long)]
        bc: bool,
    },
    /// Cook a triangle mesh's signed-distance field (M10.4a, ADR-0032 §2): the offline, cook-side
    /// half of the SDF-traced GI pipeline. Reads geometry from a glTF/GLB or binary STL source (no
    /// materials/textures — an SDF is geometry only) and writes `<name>.rsdf`.
    Sdf {
        /// A `.gltf`/`.glb`/`.stl` mesh source to build a signed-distance field from.
        input: PathBuf,
        /// Output directory for the `<name>.rsdf` file.
        #[arg(long)]
        out: PathBuf,
        /// Output file stem (writes `<name>.rsdf`). Defaults to the input file's stem.
        #[arg(long)]
        name: Option<String>,
        /// Cook at the coarse, per-destructible-part resolution preset instead of the default
        /// whole-mesh preset (a lower target resolution — see `SdfCookConfig::for_destructible_part`).
        #[arg(long)]
        coarse: bool,
    },
    /// Cook a terrain heightfield (M19.1): a 16-bit grayscale `.png` (or raw little-endian `.r16`)
    /// height map plus its `<source>.toml` sidecar (world size, height range, origin) into
    /// `<name>.rhf`. The samples are copied verbatim — the cook never re-quantises. If the sidecar
    /// names splat layers (`layer0..layer3`, material AssetIds in 0x hex), the sibling
    /// `<source stem>.splat.png` (8-bit RGBA) is cooked in as the terrain's material weights
    /// (payload v2); without layers a v1 file is written.
    Heightfield {
        /// The `.png` / `.r16` source; its sidecar is the same path with a `.toml` extension
        /// (and its optional splat map the same stem with `.splat.png`).
        input: PathBuf,
        /// Output directory for the `<name>.rhf` file.
        #[arg(long)]
        out: PathBuf,
        /// Output file stem (writes `<name>.rhf`). Defaults to the input file's stem.
        #[arg(long)]
        name: Option<String>,
    },
    /// Cook a terrain LAYER (M19.7a, ADR-0066): a `<name>.terrainlayer.toml` sidecar naming a
    /// material AssetId (0x hex), an albedo image (8-bit RGB/RGBA, opaque) and a height image (8- or
    /// 16-bit grayscale, same size; 16-bit is rounded to the nearest 8-bit level), plus
    /// `uv_scale = [x, z]` (metres per repeat in world X/Z) and `height_contrast` (>= 0). Writes
    /// `<name>_albedo_height.rtex` (RGB = albedo, A = height; uncompressed RGBA8 — BC7 later) and
    /// `<name>.rtl`, the layer record a heightfield's `layerN` may name instead of a material.
    TerrainLayer {
        /// The `<name>.terrainlayer.toml` sidecar; image paths in it are relative to it.
        input: PathBuf,
        /// Output directory for the two cooked files.
        #[arg(long)]
        out: PathBuf,
    },
    /// Cook a terrain WORLD with its LOD chain (M19.8d1, ADR-0070): a `<name>.terrainworld.toml`
    /// naming `levels` and one `tile_<x>_<z> = "source"` per level-0 tile (each a `rime
    /// heightfield` source with its own sidecar). Writes every tile as `<name>_L<level>_<x>_<z>.rhf`
    /// — level 0 cooked as `rime heightfield` cooks it, each coarser level every second sample of
    /// its four children — and the world manifest `<name>.terrainworld` (levels, bounds, geometric
    /// errors). A world the root level cannot cover exactly is refused with every reason listed.
    /// With `palette_dir = "<dir of cooked palette assets>"` it also bakes every parent's
    /// appearance (M19.8d3, ADR-0072): `<tile>_bake_color.rtex` and `<tile>_bake_material.rtex`.
    TerrainWorld {
        /// The `<name>.terrainworld.toml` description; tile paths in it are relative to it.
        input: PathBuf,
        /// Output directory for the tiles and the manifest.
        #[arg(long)]
        out: PathBuf,
    },
    /// Print the header of a cooked RMA1 asset file.
    Inspect {
        /// A cooked `.rmesh`/`.rtex` (or other RMA1) file.
        file: PathBuf,
    },
}

fn main() -> ExitCode {
    match Cli::parse().command {
        None => {
            // Preserve the Milestone-0 stub behaviour when invoked with no subcommand.
            println!("{}", banner());
            println!("Frost tooling online. Try `rime cook <input> --out <dir>` or `rime --help`.");
            ExitCode::SUCCESS
        }
        Some(Command::Cook {
            input,
            out,
            srgb: _,
            linear,
            bc,
        }) => {
            // sRGB is the default; --linear flips a texture cook to data (the flags conflict, so at
            // most one is set). Meshes ignore the colour space.
            let color_space = if linear {
                ColorSpace::Linear
            } else {
                ColorSpace::Srgb
            };
            run_cook(&input, &out, color_space, bc)
        }
        Some(Command::Fracture {
            size,
            parts,
            seed,
            out,
            name,
            damage_threshold,
            damage_scale,
        }) => run_fracture(
            &size,
            parts,
            seed,
            &out,
            &name,
            damage_threshold,
            damage_scale,
        ),
        Some(Command::Sdf {
            input,
            out,
            name,
            coarse,
        }) => run_sdf(&input, &out, name.as_deref(), coarse),
        Some(Command::Ground { out, name, bc }) => run_ground(&out, &name, bc),
        Some(Command::Heightfield { input, out, name }) => {
            run_heightfield(&input, &out, name.as_deref())
        }
        Some(Command::TerrainLayer { input, out }) => run_terrain_layer(&input, &out),
        Some(Command::TerrainWorld { input, out }) => run_terrain_world(&input, &out),
        Some(Command::Inspect { file }) => run_inspect(&file),
    }
}

#[allow(clippy::too_many_arguments)]
fn run_fracture(
    size: &[f32],
    parts: u32,
    seed: u64,
    out: &Path,
    name: &str,
    damage_threshold: f32,
    damage_scale: f32,
) -> ExitCode {
    // The CLI takes full dimensions (a 2 m wall); the fracturer works in half-extents.
    let half = [size[0] * 0.5, size[1] * 0.5, size[2] * 0.5];
    let mut cfg = asset_pipeline::fracture::FractureConfig::wall(half, parts, seed);
    cfg.damage_threshold = damage_threshold;
    cfg.damage_scale = damage_scale;
    match asset_pipeline::cook_fracture(&cfg, name, out) {
        Ok(result) => {
            for entry in &result.manifest {
                println!(
                    "cooked {} -> {} (id {:016x})",
                    entry.source_path, entry.cooked_file, entry.id
                );
            }
            ExitCode::SUCCESS
        }
        Err(e) => {
            eprintln!("rime fracture: {e}");
            ExitCode::FAILURE
        }
    }
}

/// Cook the ground's material into `out`, writing `manifest.txt` beside the cooked files so the
/// engine can resolve `ground:<name>#material0` by label.
///
/// KNOWN LIMITATION: this *writes* the manifest rather than merging into an existing one, so the
/// ground wants an output directory of its own. There is no manifest parser on the Rust side to
/// merge against, and inventing one for a directory that today holds exactly one asset would be
/// guessing at the shape of a problem no caller has yet. The brick that cooks a second asset into
/// the same directory is the one that should add the merge.
fn run_ground(out: &Path, name: &str, bc: bool) -> ExitCode {
    match asset_pipeline::cook_ground(name, out, bc) {
        Ok(result) => {
            let text = asset_pipeline::manifest::render(&result.manifest);
            if let Err(e) = std::fs::write(out.join("manifest.txt"), text) {
                eprintln!("rime ground: writing manifest.txt: {e}");
                return ExitCode::FAILURE;
            }
            for entry in &result.manifest {
                println!(
                    "cooked {} -> {} (id {:016x})",
                    entry.source_path, entry.cooked_file, entry.id
                );
            }
            ExitCode::SUCCESS
        }
        Err(e) => {
            eprintln!("rime ground: {e}");
            ExitCode::FAILURE
        }
    }
}

/// Flat triangle-soup geometry: positions plus index triples, the shape the SDF cooker speaks.
type SoupGeometry = (Vec<[f32; 3]>, Vec<[u32; 3]>);

/// Import a mesh source's raw geometry as flat `(vertices, triangles)` — the shape the SDF cooker
/// speaks (plain triangle soup), distinct from the cooker's own interleaved P/N/UV `Mesh` vertex
/// layout that `run_cook` produces. An SDF is geometry only, so this skips materials/tangents/skin
/// entirely, whichever of glTF or STL the extension names.
fn mesh_geometry_for_sdf(input: &Path) -> Result<SoupGeometry, asset_pipeline::PipelineError> {
    let ext = input
        .extension()
        .and_then(|e| e.to_str())
        .unwrap_or("")
        .to_ascii_lowercase();
    let mesh = match ext.as_str() {
        "gltf" | "glb" => {
            let primitives = asset_pipeline::gltf_import::import_primitives(input)?;
            asset_pipeline::mesh::Mesh::from_primitives(primitives)
        }
        "stl" => asset_pipeline::stl::import_stl_binary(&std::fs::read(input)?)?.mesh,
        _ => {
            return Err(asset_pipeline::PipelineError::Unsupported(format!(
                "{}: expected a .gltf/.glb/.stl mesh source",
                input.display()
            )))
        }
    };
    let vertices: Vec<[f32; 3]> = mesh.vertices.iter().map(|v| v.position).collect();
    // `as_chunks::<3>()` already yields `[u32; 3]`, so the triangle list is a copy rather than a
    // rebuild — the constant lives in the type instead of in three index expressions.
    let triangles: Vec<[u32; 3]> = mesh.indices.as_chunks::<3>().0.to_vec();
    Ok((vertices, triangles))
}

/// Cook one heightfield source into `<out>/<name>.rhf`.
fn run_heightfield(input: &Path, out: &Path, name: Option<&str>) -> ExitCode {
    let hf = match asset_pipeline::heightfield::Heightfield::from_file(input) {
        Ok(hf) => hf,
        Err(e) => {
            eprintln!("rime heightfield: {e}");
            return ExitCode::FAILURE;
        }
    };
    let stem = name.map(str::to_string).unwrap_or_else(|| {
        input
            .file_stem()
            .map(|s| s.to_string_lossy().into_owned())
            .unwrap_or_else(|| "terrain".to_string())
    });
    let (bytes, id) = hf.cook();
    let file = out.join(format!("{stem}.rhf"));
    if let Err(e) = std::fs::create_dir_all(out).and_then(|()| std::fs::write(&file, &bytes)) {
        eprintln!("rime heightfield: writing {}: {e}", file.display());
        return ExitCode::FAILURE;
    }
    println!(
        "cooked {} -> {} ({}x{} samples, {:.4} m/step, id {id:016x})",
        input.display(),
        file.display(),
        hf.columns,
        hf.rows,
        hf.height_scale()
    );
    ExitCode::SUCCESS
}

/// Cook a terrain world description into its tiles (every level) plus `<out>/<name>.terrainworld`.
fn run_terrain_world(input: &Path, out: &Path) -> ExitCode {
    let cooked = match asset_pipeline::terrain_world::cook_terrain_world(input) {
        Ok(cooked) => cooked,
        Err(e) => {
            eprintln!("rime terrain-world: {e}");
            return ExitCode::FAILURE;
        }
    };
    if let Err(e) = std::fs::create_dir_all(out) {
        eprintln!("rime terrain-world: creating {}: {e}", out.display());
        return ExitCode::FAILURE;
    }
    let manifest = (
        cooked.manifest_file_name(),
        cooked.manifest.as_bytes().to_vec(),
    );
    for (file_name, bytes) in cooked
        .files
        .iter()
        .map(|(f, b, _)| (f.clone(), b.clone()))
        .chain(std::iter::once(manifest))
    {
        let file = out.join(&file_name);
        if let Err(e) = std::fs::write(&file, bytes) {
            eprintln!("rime terrain-world: writing {}: {e}", file.display());
            return ExitCode::FAILURE;
        }
    }
    println!(
        "cooked {} -> {} ({} tiles over {} levels, root error {:.4} m)",
        input.display(),
        out.join(cooked.manifest_file_name()).display(),
        cooked.world.tiles.len(),
        cooked.world.levels,
        cooked
            .world
            .tiles
            .iter()
            .filter(|t| t.level + 1 == cooked.world.levels)
            .map(|t| t.geometric_error)
            .fold(0.0f32, f32::max)
    );
    // m19.8d3: say whether parents have an appearance — a world cooked without one still draws,
    // with the engine's placeholder material, and that should not be a surprise at run time.
    match &cooked.bake {
        Some(bake) => println!(
            "  appearance: {} parent(s) baked, {} left without (a tile beneath has no palette)",
            bake.tiles.iter().flatten().count(),
            bake.parents_unbaked
        ),
        None => println!(
            "  appearance: not baked (no `palette_dir` in the description) — parents draw with \
             the placeholder material"
        ),
    }
    ExitCode::SUCCESS
}

/// Cook one terrain layer sidecar into `<out>/<name>_albedo_height.rtex` + `<out>/<name>.rtl`.
fn run_terrain_layer(input: &Path, out: &Path) -> ExitCode {
    let cooked = match asset_pipeline::terrain_layer::cook_terrain_layer(input) {
        Ok(cooked) => cooked,
        Err(e) => {
            eprintln!("rime terrain-layer: {e}");
            return ExitCode::FAILURE;
        }
    };
    if let Err(e) = std::fs::create_dir_all(out) {
        eprintln!("rime terrain-layer: creating {}: {e}", out.display());
        return ExitCode::FAILURE;
    }
    for (file_name, (bytes, id)) in [
        (cooked.texture_file_name(), &cooked.texture),
        (cooked.layer_file_name(), &cooked.layer),
    ] {
        let file = out.join(file_name);
        if let Err(e) = std::fs::write(&file, bytes) {
            eprintln!("rime terrain-layer: writing {}: {e}", file.display());
            return ExitCode::FAILURE;
        }
        println!(
            "cooked {} -> {} (id {id:016x})",
            input.display(),
            file.display()
        );
    }
    ExitCode::SUCCESS
}

fn run_sdf(input: &Path, out: &Path, name: Option<&str>, coarse: bool) -> ExitCode {
    let (vertices, triangles) = match mesh_geometry_for_sdf(input) {
        Ok(geometry) => geometry,
        Err(e) => {
            eprintln!("rime sdf: {e}");
            return ExitCode::FAILURE;
        }
    };
    let cfg = if coarse {
        asset_pipeline::sdf::SdfCookConfig::for_destructible_part()
    } else {
        asset_pipeline::sdf::SdfCookConfig::for_mesh()
    };
    let stem = name.map(str::to_string).unwrap_or_else(|| {
        input
            .file_stem()
            .and_then(|s| s.to_str())
            .unwrap_or("mesh")
            .to_string()
    });
    match asset_pipeline::sdf::cook_mesh_sdf(&vertices, &triangles, &cfg, &stem, out) {
        Ok(result) => {
            for entry in &result.manifest {
                println!(
                    "cooked {} -> {} (id {:016x})",
                    entry.source_path, entry.cooked_file, entry.id
                );
            }
            ExitCode::SUCCESS
        }
        Err(e) => {
            eprintln!("rime sdf: {e}");
            ExitCode::FAILURE
        }
    }
}

fn run_cook(input: &Path, out: &Path, color_space: ColorSpace, bc: bool) -> ExitCode {
    match asset_pipeline::cook_path(input, out, color_space, bc) {
        Ok(result) => {
            for entry in &result.manifest {
                println!(
                    "cooked {} -> {} (id {:016x})",
                    entry.source_path, entry.cooked_file, entry.id
                );
            }
            println!(
                "{} asset(s) into {} — {} source(s) cooked, {} from cache",
                result.manifest.len(),
                out.display(),
                result.sources_cooked,
                result.sources_cached
            );
            ExitCode::SUCCESS
        }
        Err(e) => {
            eprintln!("rime cook: {e}");
            ExitCode::FAILURE
        }
    }
}

fn run_inspect(file: &Path) -> ExitCode {
    let bytes = match std::fs::read(file) {
        Ok(bytes) => bytes,
        Err(e) => {
            eprintln!("rime inspect: cannot read {}: {e}", file.display());
            return ExitCode::FAILURE;
        }
    };
    match cooked::read_header(&bytes) {
        Ok((header, _payload)) => {
            println!("{}", file.display());
            println!("  container_version : {}", header.container_version);
            println!("  asset_kind        : {}", header.asset_kind);
            println!("  type_schema_hash  : {:#018x}", header.type_schema_hash);
            println!("  payload_size      : {}", header.payload_size);
            ExitCode::SUCCESS
        }
        Err(e) => {
            eprintln!(
                "rime inspect: {} is not a valid RMA1 file: {e}",
                file.display()
            );
            ExitCode::FAILURE
        }
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    // Mirrors the C++ side's version test: pin the format (name prefix) and prove the version is
    // Cargo's, not a hand-typed literal — so the two can never drift.
    #[test]
    fn banner_has_name_and_version() {
        let b = banner();
        assert!(b.starts_with("rime-cli "));
        assert!(b.ends_with(env!("CARGO_PKG_VERSION")));
    }
}

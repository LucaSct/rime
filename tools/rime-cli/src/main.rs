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
        }) => {
            // sRGB is the default; --linear flips a texture cook to data (the flags conflict, so at
            // most one is set). Meshes ignore the colour space.
            let color_space = if linear {
                ColorSpace::Linear
            } else {
                ColorSpace::Srgb
            };
            run_cook(&input, &out, color_space)
        }
        Some(Command::Fracture {
            size,
            parts,
            seed,
            out,
            name,
        }) => run_fracture(&size, parts, seed, &out, &name),
        Some(Command::Sdf {
            input,
            out,
            name,
            coarse,
        }) => run_sdf(&input, &out, name.as_deref(), coarse),
        Some(Command::Inspect { file }) => run_inspect(&file),
    }
}

fn run_fracture(size: &[f32], parts: u32, seed: u64, out: &Path, name: &str) -> ExitCode {
    // The CLI takes full dimensions (a 2 m wall); the fracturer works in half-extents.
    let half = [size[0] * 0.5, size[1] * 0.5, size[2] * 0.5];
    let cfg = asset_pipeline::fracture::FractureConfig::wall(half, parts, seed);
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
    let triangles: Vec<[u32; 3]> = mesh
        .indices
        .chunks_exact(3)
        .map(|c| [c[0], c[1], c[2]])
        .collect();
    Ok((vertices, triangles))
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

fn run_cook(input: &Path, out: &Path, color_space: ColorSpace) -> ExitCode {
    match asset_pipeline::cook_path(input, out, color_space) {
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

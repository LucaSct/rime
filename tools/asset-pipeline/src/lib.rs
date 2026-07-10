// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 The Rime Engine Authors.

//! Rime's offline asset pipeline: import source assets and **cook** them into the engine's runtime
//! RMA1 format (ADR-0024). This crate is the *writer* of record for the cooked container; the C++
//! `engine/assets` module is the reader. Files are the boundary (ADR-0001) — this crate never links
//! the engine, and the engine never parses glTF.
//!
//! M6.2 covers glTF **mesh** import → cook (`rime-cli cook`). Textures (M6.3), materials/tangents
//! (M6.4), and other source formats (STL at M6.6) are later bricks that reuse `cooked`/`manifest`.

pub mod cooked;
pub mod gltf_import;
pub mod gltf_material;
pub mod manifest;
pub mod material;
pub mod math;
pub mod mesh;
pub mod tangent;
pub mod texture;

use std::fmt;
use std::path::{Path, PathBuf};

use manifest::ManifestEntry;
use mesh::Mesh;
use texture::{ColorSpace, Texture};

/// Anything that can go wrong cooking an asset.
#[derive(Debug)]
pub enum PipelineError {
    /// The glTF importer failed (parse error, bad buffer, unsupported feature).
    Gltf(gltf::Error),
    /// A texture failed to decode (corrupt/unsupported PNG or JPEG).
    Image(image::ImageError),
    /// A filesystem error reading a source or writing a cooked file.
    Io(std::io::Error),
    /// The source is valid but uses something v1 does not cook (with a human message).
    Unsupported(String),
}

impl fmt::Display for PipelineError {
    fn fmt(&self, f: &mut fmt::Formatter<'_>) -> fmt::Result {
        match self {
            PipelineError::Gltf(e) => write!(f, "glTF import error: {e}"),
            PipelineError::Image(e) => write!(f, "image decode error: {e}"),
            PipelineError::Io(e) => write!(f, "I/O error: {e}"),
            PipelineError::Unsupported(msg) => write!(f, "unsupported: {msg}"),
        }
    }
}

impl std::error::Error for PipelineError {
    fn source(&self) -> Option<&(dyn std::error::Error + 'static)> {
        match self {
            PipelineError::Gltf(e) => Some(e),
            PipelineError::Image(e) => Some(e),
            PipelineError::Io(e) => Some(e),
            PipelineError::Unsupported(_) => None,
        }
    }
}

impl From<gltf::Error> for PipelineError {
    fn from(e: gltf::Error) -> Self {
        PipelineError::Gltf(e)
    }
}

impl From<image::ImageError> for PipelineError {
    fn from(e: image::ImageError) -> Self {
        PipelineError::Image(e)
    }
}

impl From<std::io::Error> for PipelineError {
    fn from(e: std::io::Error) -> Self {
        PipelineError::Io(e)
    }
}

/// What a cook produced: the cooked files written and the manifest entries describing them.
#[derive(Default)]
pub struct CookOutput {
    pub cooked_files: Vec<PathBuf>,
    pub manifest: Vec<ManifestEntry>,
}

/// Import (and cook) a glTF's materials + textures from a path — the path-based convenience over
/// `gltf_material::import_materials`, symmetric with `gltf_import::import_primitives`. Handy for tools
/// and tests that want the structured material import without driving the whole file cook.
pub fn import_gltf_materials(path: &Path) -> Result<gltf_material::MaterialImport, PipelineError> {
    let (document, _buffers, images) = gltf::import(path)?;
    let stem = path.file_stem().and_then(|s| s.to_str()).unwrap_or("mesh");
    gltf_material::import_materials(&document, &images, stem)
}

/// Cook a single glTF file into `out_dir`. All of the file's triangle primitives merge into one RMA1
/// mesh (one submesh per primitive, tagged with a material slot) written as `<stem>.rmesh`; each
/// material becomes a `<stem>.mat<N>.rmat`, and each unique texture a `<stem>.img<N>.<srgb|lin>.rtex`
/// (M6.4). Meshes whose material carries a normal map cook tangents. Returns every cooked file plus
/// the manifest entries describing them.
pub fn cook_gltf(input: &Path, out_dir: &Path) -> Result<CookOutput, PipelineError> {
    // One decode of the glTF, shared by the material import (which needs the decoded images) and the
    // geometry walk (which needs the buffers) — importing twice would re-decode every texture.
    let (document, buffers, images) = gltf::import(input)?;
    let stem = input.file_stem().and_then(|s| s.to_str()).unwrap_or("mesh");

    // Materials first: whether a primitive's material carries a normal map decides if the mesh needs
    // tangents, and that has to be known before the mesh is cooked.
    let mut mats = gltf_material::import_materials(&document, &images, stem)?;

    let primitives = gltf_import::primitives_from(&document, &buffers)?;
    let mut mesh = Mesh::from_primitives(primitives);
    if mesh.is_empty() {
        return Err(PipelineError::Unsupported(format!(
            "{} yielded no triangle mesh data",
            input.display()
        )));
    }

    // Materialize glTF's default material if any primitive used it (slot == the count of defined
    // materials), so every submesh slot resolves to a real cooked material. A file with no materials
    // at all (the plain quad fixture) lands here too: one default material at slot 0.
    let default_slot = mats.materials.len() as u32;
    if mesh
        .submeshes
        .iter()
        .any(|s| s.material_slot == default_slot)
    {
        let default = material::Material::default();
        let (bytes, id) = default.cook();
        mats.materials.push(gltf_material::CookedMaterial {
            id,
            bytes,
            material: default,
            has_normal_map: false,
        });
    }

    // Tangent policy (M6.4): one vertex layout per mesh, so if ANY submesh's material needs a normal
    // map, generate tangents for the whole mesh; a mesh no normal map touches stays 32-byte P/N/UV.
    let needs_tangents = mesh.submeshes.iter().any(|s| {
        mats.materials
            .get(s.material_slot as usize)
            .is_some_and(|m| m.has_normal_map)
    });
    if needs_tangents {
        tangent::generate_tangents(&mut mesh);
    }

    let (mesh_bytes, mesh_id) = mesh.cook();

    std::fs::create_dir_all(out_dir)?;
    let mut out = CookOutput::default();

    let mesh_file = format!("{stem}.rmesh");
    std::fs::write(out_dir.join(&mesh_file), &mesh_bytes)?;
    out.cooked_files.push(out_dir.join(&mesh_file));
    out.manifest.push(ManifestEntry {
        source_path: input.to_string_lossy().into_owned(),
        kind: "mesh",
        id: mesh_id,
        cooked_file: mesh_file,
    });

    for (i, m) in mats.materials.iter().enumerate() {
        let file = format!("{stem}.mat{i}.rmat");
        std::fs::write(out_dir.join(&file), &m.bytes)?;
        out.cooked_files.push(out_dir.join(&file));
        out.manifest.push(ManifestEntry {
            source_path: format!("{}#material{i}", input.display()),
            kind: "material",
            id: m.id,
            cooked_file: file,
        });
    }

    for t in &mats.textures {
        std::fs::write(out_dir.join(&t.cooked_file), &t.bytes)?;
        out.cooked_files.push(out_dir.join(&t.cooked_file));
        out.manifest.push(ManifestEntry {
            source_path: t.source_label.clone(),
            kind: "texture",
            id: t.id,
            cooked_file: t.cooked_file.clone(),
        });
    }

    Ok(out)
}

/// Cook a single PNG/JPEG file into `out_dir` as `<stem>.rtex`: decode to RGBA8, generate an offline
/// mip chain (gamma-correctly for `Srgb`), and write the RMA1 texture. Returns its path + manifest
/// entry. `color_space` says whether the image is colour (sRGB) or data (linear) — the CLI's
/// `--srgb`/`--linear`; from M6.4 a material's usage of the texture picks it automatically.
pub fn cook_texture(
    input: &Path,
    out_dir: &Path,
    color_space: ColorSpace,
) -> Result<CookOutput, PipelineError> {
    let texture = Texture::from_file(input, color_space)?;
    let (bytes, id) = texture.cook();

    let stem = input
        .file_stem()
        .and_then(|s| s.to_str())
        .unwrap_or("texture");
    let cooked_file = format!("{stem}.rtex");
    std::fs::create_dir_all(out_dir)?;
    let cooked_path = out_dir.join(&cooked_file);
    std::fs::write(&cooked_path, &bytes)?;

    Ok(CookOutput {
        cooked_files: vec![cooked_path],
        manifest: vec![ManifestEntry {
            source_path: input.to_string_lossy().into_owned(),
            kind: "texture",
            id,
            cooked_file,
        }],
    })
}

/// The kinds of source file the pipeline cooks today. Meshes are glTF; textures are PNG/JPEG.
#[derive(Clone, Copy)]
enum SourceKind {
    Mesh,
    Texture,
}

/// Classify a source file by its (lower-cased) extension, or `None` if it is not something we cook —
/// so a directory cook skips stray files and a single-file cook of an unknown type errors cleanly.
fn cook_kind_for(path: &Path) -> Option<SourceKind> {
    match path
        .extension()
        .and_then(|e| e.to_str())?
        .to_ascii_lowercase()
        .as_str()
    {
        "gltf" | "glb" => Some(SourceKind::Mesh),
        "png" | "jpg" | "jpeg" => Some(SourceKind::Texture),
        _ => None,
    }
}

/// Cook one source file, dispatching on its extension. `color_space` applies to textures only.
fn cook_one(
    input: &Path,
    out_dir: &Path,
    color_space: ColorSpace,
) -> Result<CookOutput, PipelineError> {
    match cook_kind_for(input) {
        Some(SourceKind::Mesh) => cook_gltf(input, out_dir),
        Some(SourceKind::Texture) => cook_texture(input, out_dir, color_space),
        None => Err(PipelineError::Unsupported(format!(
            "{}: unrecognised source extension (expected .gltf/.glb/.png/.jpg/.jpeg)",
            input.display()
        ))),
    }
}

/// Cook a single source file or every cookable file in a directory, then write a `manifest.txt` into
/// `out_dir`. Directory entries are cooked in sorted order so the run is deterministic. For textures,
/// `color_space` (the CLI's `--srgb`/`--linear`) says whether the image is colour or data; meshes
/// ignore it.
pub fn cook_path(
    input: &Path,
    out_dir: &Path,
    color_space: ColorSpace,
) -> Result<CookOutput, PipelineError> {
    let mut inputs: Vec<PathBuf> = if input.is_dir() {
        std::fs::read_dir(input)?
            .filter_map(|entry| entry.ok().map(|e| e.path()))
            .filter(|p| cook_kind_for(p).is_some())
            .collect()
    } else {
        vec![input.to_path_buf()]
    };
    inputs.sort();

    let mut combined = CookOutput::default();
    for source in &inputs {
        let out = cook_one(source, out_dir, color_space)?;
        combined.cooked_files.extend(out.cooked_files);
        combined.manifest.extend(out.manifest);
    }

    std::fs::create_dir_all(out_dir)?; // ensure the manifest can be written even for an empty dir
    std::fs::write(
        out_dir.join("manifest.txt"),
        manifest::render(&combined.manifest),
    )?;
    Ok(combined)
}

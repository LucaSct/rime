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
pub mod manifest;
pub mod math;
pub mod mesh;

use std::fmt;
use std::path::{Path, PathBuf};

use manifest::ManifestEntry;
use mesh::Mesh;

/// Anything that can go wrong cooking an asset.
#[derive(Debug)]
pub enum PipelineError {
    /// The glTF importer failed (parse error, bad buffer, unsupported feature).
    Gltf(gltf::Error),
    /// A filesystem error reading a source or writing a cooked file.
    Io(std::io::Error),
    /// The source is valid glTF but uses something v1 does not cook (with a human message).
    Unsupported(String),
}

impl fmt::Display for PipelineError {
    fn fmt(&self, f: &mut fmt::Formatter<'_>) -> fmt::Result {
        match self {
            PipelineError::Gltf(e) => write!(f, "glTF import error: {e}"),
            PipelineError::Io(e) => write!(f, "I/O error: {e}"),
            PipelineError::Unsupported(msg) => write!(f, "unsupported: {msg}"),
        }
    }
}

impl std::error::Error for PipelineError {
    fn source(&self) -> Option<&(dyn std::error::Error + 'static)> {
        match self {
            PipelineError::Gltf(e) => Some(e),
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

/// Cook a single glTF file into `out_dir`. All of the file's triangle primitives merge into one RMA1
/// mesh (one submesh per primitive), written as `<stem>.rmesh`. Returns its path + manifest entry.
pub fn cook_gltf(input: &Path, out_dir: &Path) -> Result<CookOutput, PipelineError> {
    let primitives = gltf_import::import_primitives(input)?;
    let mesh = Mesh::from_primitives(primitives);
    if mesh.is_empty() {
        return Err(PipelineError::Unsupported(format!(
            "{} yielded no triangle mesh data",
            input.display()
        )));
    }
    let (bytes, id) = mesh.cook();

    let stem = input.file_stem().and_then(|s| s.to_str()).unwrap_or("mesh");
    let cooked_file = format!("{stem}.rmesh");
    std::fs::create_dir_all(out_dir)?;
    let cooked_path = out_dir.join(&cooked_file);
    std::fs::write(&cooked_path, &bytes)?;

    Ok(CookOutput {
        cooked_files: vec![cooked_path],
        manifest: vec![ManifestEntry {
            source_path: input.to_string_lossy().into_owned(),
            kind: "mesh",
            id,
            cooked_file,
        }],
    })
}

/// Cook a single glTF file or every `.gltf`/`.glb` in a directory, then write a `manifest.txt` into
/// `out_dir`. Directory entries are cooked in sorted order so the run is deterministic.
pub fn cook_path(input: &Path, out_dir: &Path) -> Result<CookOutput, PipelineError> {
    let mut inputs: Vec<PathBuf> = if input.is_dir() {
        std::fs::read_dir(input)?
            .filter_map(|entry| entry.ok().map(|e| e.path()))
            .filter(|p| {
                matches!(
                    p.extension().and_then(|e| e.to_str()),
                    Some("gltf") | Some("glb")
                )
            })
            .collect()
    } else {
        vec![input.to_path_buf()]
    };
    inputs.sort();

    let mut combined = CookOutput::default();
    for source in &inputs {
        let out = cook_gltf(source, out_dir)?;
        combined.cooked_files.extend(out.cooked_files);
        combined.manifest.extend(out.manifest);
    }

    std::fs::write(
        out_dir.join("manifest.txt"),
        manifest::render(&combined.manifest),
    )?;
    Ok(combined)
}

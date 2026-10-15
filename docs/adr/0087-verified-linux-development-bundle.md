# ADR-0087: Verified Linux development bundle

Status: accepted, 2026-10-15.

## Context

The existing hello-game export could run without the repository or Vulkan, but
did not identify every shipped byte or retain reviewed redistribution notices.
An export is a development artifact until its compatibility and release gates
are independently proven.

## Decision

Keep the existing Linux exporter and content-root contract. Add a strict,
versioned TOML inventory with sorted relative paths, sizes, execution modes and
SHA-256 hashes. Record the actual linked executable's build-time source revision
and dirty flag, measured ELF ABI requirements, supported modes and content
identity. Unknown/stale provenance and unreviewed dependency versions refuse
export. The current profile is Linux x86_64 hello-game only.

Stage privately, verify, then publish by rename; failed replacement preserves
the prior bundle. Include authentic dependency notices from the reviewed Conan
packages and generated Wayland protocol sources. Reject links, special files,
unsafe paths, extra/missing payloads, altered bytes and missing notices.

The isolation proof copies a trusted verifier and pins the manifest digest before
hiding HOME, repository, cache and Vulkan. It mounts only the artifact read-only
at a separate path. A checkout outside HOME also requires hiding its parent.
Keep the existing dedicated simulation digest comparisons and failure controls;
add modified-content and missing-notice controls before artifact upload.

## Evidence and limits

32 Python cases pass. Release export proofs pass with both HOME-based and
outside-HOME checkout layouts, including actual isolated executable runs.
CI adds a mandatory nextos export job because the starbase runner's existing LXC
does not permit nested namespaces. It retains logs on failure and uploads the
bundle plus proof only on success.

Unsigned hashes identify bytes, not a publisher. Dedicated mode is currently a
GPU-free simulation, not a listening multiplayer server. No clean second-machine
or production-release claim follows: the current host requires GLIBC 2.43.
Choose a release baseline and prove compatibility separately. Windows, consoles,
browser runtime and exported multiplayer remain subsequent bricks.

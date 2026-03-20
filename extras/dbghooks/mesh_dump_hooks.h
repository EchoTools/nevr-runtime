#pragma once

#include "pch.h"

/// <summary>
/// Initializes the mesh dumping hooks for CGMeshListResource analysis.
///
/// This implements hooks to capture mesh binary data at runtime:
/// 1. AsyncResourceIOCallback RVA 0x0fa16d0 (absolute: 0x140fa16d0) - Dumps raw sub-asset files (.meshes,
/// .vertexbuffers, etc.)
/// 2. CGMeshListResource::DeserializeAndUpload RVA 0x0547ab0 (absolute: 0x140547ab0) - Dumps parsed mesh structures
///
/// Output directory: ./mesh_dumps/
/// - raw_buffers/ - Raw binary files from I/O system
/// - parsed_meshes/ - Parsed vertex/index data with descriptors
/// - mesh_log.txt - Metadata and structure information
///
/// Documented in:
/// - evr-reconstruction/docs/kb/cgmeshlist_binary_format_discovered
/// - evr-reconstruction/docs/MESH_BINARY_FORMAT_RESEARCH.md
/// </summary>
VOID InitializeMeshDumpHooks();

/// <summary>
/// Shuts down the mesh dump hooks and flushes log files.
/// </summary>
VOID ShutdownMeshDumpHooks();

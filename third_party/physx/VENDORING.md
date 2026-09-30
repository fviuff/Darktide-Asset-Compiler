# PhysX source provenance

The existing `../VERSIONS.lock` pin remains PhysX 4.1.2 commit
`a2c0428acab643e60618c681b501e86f7fd558cc`.

The full runtime source, headers, compiler/build helpers, and
`externals/cmakemodules` were added from that exact upstream archive:

`https://codeload.github.com/NVIDIAGameWorks/PhysX/zip/a2c0428acab643e60618c681b501e86f7fd558cc`

Archive SHA-256: `d9c1939490a990277f8c773f288294cecb10e6fad8c820acad90fd4168b8ace3`.
Samples, documentation media, prebuilt DLLs, and native game data are not required.
This is an expansion of the existing pinned source subset, not a version change.

Previously present sources were preserved. Apart from CRLF line endings, the
existing local differences are `PxPreprocessor.h` (Windows Clang packing guard),
`PsWindowsThread.cpp` (MinGW diagnostic thread-name guard), and generated
`PxConfig.h` (static linkage). The upstream CMake also regenerates `PxConfig.h`
for static linkage when configuring the MSVC build.

MSVC builds use one upstream static SDK graph. The existing non-MSVC
Foundation/Common/Cooking graph remains separate; both graphs are never linked
into the same compiler. The application's CRT and the SDK CRT are both static.
See `../../cmake/PhysXRuntime.cmake` for consuming build settings.

The full MSVC runtime enables Intel SIMD: PhysX's scalar configuration can cook
BVH34 meshes but reports an unsupported BV4 midphase when simulating contact
against them. This distinction is covered by the collection writer's cooked
triangle-floor drop test.

Local initialization fix (20 September 2026):
`physx/source/physx/src/NpConnector.h` initializes the three explicit padding
bytes in its constructors and Release copy constructor. The SDK's deterministic
binary converter preserves these bytes in inline connector arrays; the upstream
Release constructor otherwise copies stack residue into output. Layout, fields,
serialization GUID and checked-build `markSerializedMem` behavior are preserved.
The writer test serializes identical connected bodies twice and compares bytes.

`physx/source/geomutils/src/GuMetaData.cpp` also describes `BV4Tree::mNodes` as a
pointer for binary conversion. Upstream omits it, so conversion fills the field
as padding. For a small mesh with zero BV4 nodes, the unchanged game importer
does not replace that field; it must remain NULL. This metadata correction
preserves the field layout and emits a valid NULL. Populated trees still use
the existing extra-data importer. Tests simulate contact against both a single
triangle and a 128-triangle floor after deterministic serialization/reload.

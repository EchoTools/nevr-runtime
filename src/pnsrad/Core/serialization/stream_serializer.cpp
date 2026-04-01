// @module: pnsrad.dll
// @source: d:\projects\rad\dev\src\engine\libs\io\cserializer.cpp
// @confidence: H — from Ghidra decompilation

#include "src/pnsrad/Core/serialization/stream_serializer.h"

namespace NRadEngine {

// @0x1800a3e10
CStreamSerializer::~CStreamSerializer() {
    // Ghidra: sets vtable to CSerializer::vftable, then checks (param_1 & 1) for delete.
    // In C++ the vtable reset and conditional delete happen implicitly.
}

} // namespace NRadEngine

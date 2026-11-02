// NRadEngine::CQuaternion::FromRotationMatrix
// Quest libr15.so VA 0x140ad54; ReVault binary 576.
// ReVault raw parent 100226614, reconstruction node 100600585; grade M: all 17 instructions checked and the 36-byte transposed copy is reproduced, but the call's thunk (0xfe5110) GOT slot 0x37c9f70 reads 0x00ecfb40 in the stored bytes, so the FromRotationMatrixInv callee is Ghidra-resolved and not byte-checked.
// @0x140ad54 — NRadEngine::CQuaternion::FromRotationMatrix
/* @confidence: M */

namespace NRadEngine {

// Row-major 3x3 matrix of nine floats (36 bytes). FromRotationMatrixInv reads it as float[9] (in_x1[0..8]).
struct C33Matrix {
    float m[9];
};
static_assert(sizeof(C33Matrix) == 0x24);

struct CQuaternion {
    // x, y, z, w at +0x00..+0x0c; written by FromRotationMatrixInv, not touched by this function.
    float v[4];

    // NRadEngine::CQuaternion::FromRotationMatrixInv(NRadEngine::C33Matrix const&), VA 0x1426288; the call goes through thunk 0xfe5110.
    void FromRotationMatrixInv(const C33Matrix& basis);
    // NRadEngine::CQuaternion::FromRotationMatrix(NRadEngine::C33Matrix const&), VA 0x140ad54, thunk stub 0x1005370.
    void FromRotationMatrix(const C33Matrix& basis);
};
static_assert(sizeof(CQuaternion) == 0x10);

// this (x0) is passed through unchanged; basis is x1. The stack copy at sp+0x08 is the transpose of basis.
void CQuaternion::FromRotationMatrix(const C33Matrix& basis) {
    C33Matrix transposed;
    transposed.m[0] = basis.m[0];
    transposed.m[1] = basis.m[3];
    transposed.m[2] = basis.m[6];
    transposed.m[3] = basis.m[1];
    transposed.m[4] = basis.m[4];
    transposed.m[5] = basis.m[7];
    transposed.m[6] = basis.m[2];
    transposed.m[7] = basis.m[5];
    transposed.m[8] = basis.m[8];
    FromRotationMatrixInv(transposed);
}

}  // namespace NRadEngine

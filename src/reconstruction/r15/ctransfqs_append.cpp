// @0x14302b8 — CTransfQS::Append(CTransfQS const&)
// Quest libr15.so, binary 576; raw parent 100227260; reconstruction node 100600324.
/* @confidence: H */

namespace q15t::r15::reconstruction {

struct CTransfQS {
    float value[12];
    void Append(const CTransfQS& other);
};

void CTransfQS::Append(const CTransfQS& other) {
    const float self_tx = value[4];
    const float self_ty = value[5];
    const float self_tz = value[6];
    const float other_qx = other.value[0];
    const float other_qy = other.value[1];
    const float other_qz = other.value[2];
    const float other_qw = other.value[3];

    float cross_z = self_tz * other_qy - self_ty * other_qz;
    float cross_x = self_ty * other_qx - self_tx * other_qy;
    float cross_y = self_tx * other_qz - self_tz * other_qx;
    cross_z += cross_z;
    cross_x += cross_x;
    cross_y += cross_y;

    const float scale_x = other.value[7];
    const float scale_y = other.value[8];
    const float scale_z = other.value[9];
    const float other_tx = other.value[4];
    const float other_ty = other.value[5];
    const float other_tz = other.value[6];

    value[4] = other_tx + scale_x *
        ((self_tx + other_qw * cross_z + other_qy * cross_x) -
         other_qz * cross_y);
    value[5] = other_ty + scale_y *
        ((self_ty + other_qz * cross_z + other_qw * cross_y) -
         other_qx * cross_x);
    value[6] = other_tz + scale_z *
        ((self_tz - other_qy * cross_z) + other_qx * cross_y +
         other_qw * cross_x);

    const float self_qx = value[0];
    const float self_qy = value[1];
    const float self_qz = value[2];
    const float self_qw = value[3];
    const float qx = other_qx;
    const float qy = other_qy;
    const float qz = other_qz;
    const float qw = other_qw;

    value[0] = ((qx * self_qw + qw * self_qx) - self_qy * qz) +
               qy * self_qz;
    value[1] = (qy * self_qw + qw * self_qy + self_qx * qz) -
               qx * self_qz;
    value[2] = (self_qy * qx - qy * self_qx) + self_qw * qz +
               qw * self_qz;
    value[3] = self_qw * qw -
               (self_qx * qx + self_qy * qy + self_qz * qz);

    value[7] *= other.value[7];
    value[8] *= other.value[8];
    value[9] *= other.value[9];
}

} // namespace q15t::r15::reconstruction

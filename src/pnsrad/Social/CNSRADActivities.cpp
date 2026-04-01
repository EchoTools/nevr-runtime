#include "CNSRADActivities.h"

#include <cstring>

/* @module: pnsrad.dll */
/* @purpose: CNSRAD Activities implementation */

/* @addr: 0x180013000 (pnsrad.dll) */
/* @original: CNSRADActivities::CNSRADActivities */
/* @confidence: H */
CNSRADActivities::CNSRADActivities() {
    // @0x18007f1e0 — CNSIActivities base constructor, then CNSRADActivities vtable set
    // Sets CNSIActivities::vftable, zeros 0x24 qwords (offsets 1-0x24),
    // then sets CNSRADActivities::vftable. Stores context_ptr at +0x25*8.
    // Initializes 3 sub-structures via fcn_18007f500 at offsets +0x26, +0x37, +0x48.
    memset(data_, 0, sizeof(data_));
    // vtable = &NRadEngine::CNSRADActivities::vftable; (set by caller after return)
    // data_[0x25*8] = context_ptr; (set by caller)
    // fcn_18007f500(&data_[0x26*8]); — sub-structure init (activity tracker 1)
    // fcn_18007f500(&data_[0x37*8]); — sub-structure init (activity tracker 2)
    // fcn_18007f500(&data_[0x48*8]); — sub-structure init (activity tracker 3)
}

/* @addr: 0x180013100 (pnsrad.dll) */
/* @original: CNSRADActivities::~CNSRADActivities */
/* @confidence: H */
CNSRADActivities::~CNSRADActivities() {
    // Cleanup: destroy 3 sub-structures at +0x26, +0x37, +0x48 (reverse order)
    // Then base class CNSIActivities destructor zeros fields
}

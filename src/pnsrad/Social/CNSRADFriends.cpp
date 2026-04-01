#include "CNSRADFriends.h"

#include <cstring>

/* @module: pnsrad.dll */
/* @purpose: CNSRAD Friends implementation */

/* @addr: 0x180011000 (pnsrad.dll) */
/* @original: CNSRADFriends::CNSRADFriends */
/* @confidence: H */
CNSRADFriends::CNSRADFriends() {
    // @0x18007f2e0 — constructor: calls CNSIFriends base init (fcn_18007f100),
    // then sets CNSRADFriends::vftable. Stores context_ptr at +0x2d*8.
    // Initializes 6 BufferContext structures (capacity 0x20 each) at:
    //   +0x30, +0x37, +0x4c, +0x53, +0x68, +0x6f (qword offsets)
    // Plus 3 sub-structures via fcn_18007f480 at +0x3e, +0x5a, +0x76.
    // Zeros 16 bytes at +0x2e via fcn_1800897f0.
    memset(data_, 0, sizeof(data_));
    // vtable = &NRadEngine::CNSRADFriends::vftable; (set by caller)
}

/* @addr: 0x180011100 (pnsrad.dll) */
/* @original: CNSRADFriends::~CNSRADFriends */
/* @confidence: H */
CNSRADFriends::~CNSRADFriends() {
    // Cleanup: destroy 6 BufferContext structures and 3 sub-structures (reverse order)
    // Then base class CNSIFriends destructor
}

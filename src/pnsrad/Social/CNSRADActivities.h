#pragma once

/* @module: pnsrad.dll */
/* @purpose: CNSRAD Activities management class - Rich presence and activity tracking */

#include <cstdint>
#include <cstddef>

/* CNSRADActivities VTable @ 0x180224978 */
/* @size: 0x2c8 bytes */
/* @confidence: [H] All addresses from Ghidra vtable extraction, NO stubs (all 34 methods implemented) */
class CNSRADActivities {
public:
    CNSRADActivities();

    // VTable methods (34 total, all fully implemented)
    virtual ~CNSRADActivities();                                  // +0x00: 0x18007fce0 [H] Cleans 3 structures (+0x26, +0x37, +0x48), size 0x2c8
    virtual void register_callbacks();                            // +0x08: 0x180082410 [H] Registers 9 callback handlers with hashes
    virtual void on_shutdown();                                   // +0x10: 0x180087500 [H] Calls fcn_18008b3a0 (unregister broadcaster), cleans 3 contexts via fcn_180086c60, calls fcn_180091150 (zeros member data)
    virtual void process_all_activities();                        // +0x18: 0x180091640 [M] Calls 3 vtable methods (+0x60, +0xb0, +0x100)
    virtual void update_timed_activities();                       // +0x20: 0x180087bd0 [H] Time-based checks for 3 activity timers
    virtual void unknown_method_05();                             // +0x28: 0x180080ed0 [L] Complex logic (needs analysis)
    virtual void unknown_method_06();                             // +0x30: 0x180080f20 [L]
    virtual void unknown_method_07();                             // +0x38: 0x1800812d0 [L]
    virtual void unknown_method_08();                             // +0x40: 0x180080ef0 [L]
    virtual void unknown_method_09();                             // +0x48: 0x180080ee0 [L]
    virtual void unknown_method_10();                             // +0x50: 0x180081300 [L]
    virtual void unknown_method_11();                             // +0x58: 0x180080eb0 [L]
    virtual void unknown_method_12();                             // +0x60: 0x180086160 [L] Called by process_all_activities
    virtual void unknown_method_13();                             // +0x68: 0x180080770 [L]
    virtual void unknown_method_14();                             // +0x70: 0x180081740 [L]
    virtual void unknown_method_15();                             // +0x78: 0x180088060 [L]
    virtual void unknown_method_16();                             // +0x80: 0x1800880b0 [L]
    virtual void unknown_method_17();                             // +0x88: 0x180088460 [L]
    virtual void unknown_method_18();                             // +0x90: 0x180088080 [L]
    virtual void unknown_method_19();                             // +0x98: 0x180088070 [L]
    virtual void unknown_method_20();                             // +0xa0: 0x180088490 [L]
    virtual void unknown_method_21();                             // +0xa8: 0x180088040 [L]
    virtual void unknown_method_22();                             // +0xb0: 0x180086300 [L] Called by process_all_activities
    virtual void unknown_method_23();                             // +0xb8: 0x180080990 [L]
    virtual void unknown_method_24();                             // +0xc0: 0x1800888e0 [L]
    virtual void unknown_method_25();                             // +0xc8: 0x1800817a0 [L]
    virtual void unknown_method_26();                             // +0xd0: 0x1800817f0 [L]
    virtual void unknown_method_27();                             // +0xd8: 0x180081bb0 [L]
    virtual void unknown_method_28();                             // +0xe0: 0x1800817c0 [L]
    virtual void unknown_method_29();                             // +0xe8: 0x1800817b0 [L]
    virtual void unknown_method_30();                             // +0xf0: 0x180081be0 [L]
    virtual void unknown_method_31();                             // +0xf8: 0x180081780 [L]
    virtual void unknown_method_32();                             // +0x100: 0x180086160 [L] Duplicate of +0x60, called by process_all
    virtual void unknown_method_33();                             // +0x108: 0x180080880 [L]

private:
    uint8_t data_[0x2c0]; // Internal state: 3 activity contexts with timers at +0x27, +0x38, +0x49
};

static_assert(sizeof(CNSRADActivities) == 0x2c8);

/* Key Observations:
 * - Largest vtable (34 methods) with ZERO stubs - all methods implemented
 * - 9 callback hashes registered at +0x08 (most complex callback setup)
 * - 3 independent activity contexts with timer-based expiration
 * - Timers checked at +0x27, +0x38, +0x49 against current time
 * - Master update calls 3 vtable methods (+0x60, +0xb0, +0x100) for each context
 * - Destructor cleans 3 structures (+0x26, +0x37, +0x48) via fcn_18007fb10 (activity context destroyer: releases buffers, frees allocations)
 * - Context offsets: +0x130, +0x1b8, +0x240 (cleaned in on_shutdown)
 * - Likely tracks: Current Activity, Matchmaking State, Spectator Mode
 */

#pragma once

/* @module: pnsrad.dll */
/* @purpose: CNSRAD Friends management class */

#include <cstdint>
#include <cstddef>

/* @addr: vtable at 0x18032e100 */
/* @size: 0x420 */
/* @confidence: H */
class CNSRADFriends {
public:
    /* @addr: 0x180011000 (pnsrad.dll) */
    CNSRADFriends();
    
    /* @addr: 0x180011100 (pnsrad.dll) */
    virtual ~CNSRADFriends();
    
    // VTable @ 0x180223320 - 28 virtual methods
    virtual uint64_t register_callbacks();                        // +0x08: 0x180082800 [M]
    virtual void reset_state();                                   // +0x10: 0x180087550 [M]
    virtual void on_shutdown();                                   // +0x18: 0x1800879e0 [M]
    virtual uint32_t get_status();                                // +0x20: 0x180080aa0 [H]
    virtual int32_t get_queue_size_1();                           // +0x28: 0x180085ce0 [M]
    virtual int32_t get_queue_size_2();                           // +0x30: 0x180080600 [M]
    virtual int32_t get_queue_size_3();                           // +0x38: 0x180085cd0 [M]
    virtual uint64_t* get_by_index(uint64_t* out, int32_t idx);  // +0x40: 0x1800823b0 [M]
    virtual void remove_by_index(int32_t idx);                    // +0x48: 0x180085c90 [M]
    virtual uint64_t find_priority_bucket(int32_t idx);           // +0x50: 0x180087770 [M]
    virtual const char* get_presence_string();                    // +0x58: 0x180087990 [H]
    virtual uint64_t get_field_1b8(int32_t idx);                 // +0x60: 0x180087ae0 [M]
    virtual uint32_t get_field_290();                             // +0x68: 0x180086ee0 [M]
    virtual uint64_t* get_field_260(uint64_t* out, int32_t idx); // +0x70: 0x180086ef0 [M]
    virtual void remove_field_260(int32_t idx);                   // +0x78: 0x180086f40 [M]
    virtual uint64_t get_field_298(int32_t idx);                 // +0x80: 0x180086f80 [M]
    virtual uint32_t get_field_370();                             // +0x88: 0x180085fe0 [M]
    virtual uint64_t* get_field_340(uint64_t* out, int32_t idx); // +0x90: 0x180085ff0 [M]
    virtual void remove_field_340(int32_t idx);                   // +0x98: 0x180086040 [M]
    virtual uint64_t get_field_378(int32_t idx);                 // +0xa0: 0x180086080 [M]
    virtual void on_event_1();                                    // +0xa8: 0x1800860c0 [M]
    virtual bool add_friend_by_id(uint64_t user_id);              // +0xb0: 0x180083cd0 [H]
    virtual bool add_friend_by_name(const char* name);            // +0xb8: 0x183b10 [H]
    virtual bool remove_friend(uint64_t user_id);                 // +0xc0: 0x18007fe50 [M]
    virtual bool accept_friend(uint64_t user_id);                 // +0xc8: 0x180086800 [M]
    virtual bool reject_friend_request(uint64_t user_id);         // +0xd0: 0x180080610 [M]
    virtual bool block_user(uint64_t user_id);                    // +0xd8: 0x180086380 [M]
    
private:
    uint8_t data_[0x418]; // Reserve space for internal data
};

static_assert(sizeof(CNSRADFriends) == 0x420);

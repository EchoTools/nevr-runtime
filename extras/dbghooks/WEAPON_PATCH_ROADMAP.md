# Weapon Visual Effects Fix - Complete Roadmap

## Current Status

### ✅ COMPLETED
1. **Gun2CR Patch (Primary Fix)**
   - Location: `~/src/nevr-server/dbghooks/gun2cr_fix/`
   - Status: COMPLETE & VERIFIED (10/10 checks)
   - Files: gun2cr_hook.cpp/h, loader.h/cpp, MANIFEST.yaml, README.md, CMakeLists.txt, config/autoload.yaml
   - Ready: YES - Can build and deploy immediately
   - Testing: Requires in-game verification

2. **Pattern Analysis**
   - Root cause identified: Zero-initialization of SProperties struct
   - Hook point identified: CR15NetBullet2CS::InitBulletCI @ 0x140f991e0
   - Working reference: GunCR (CR15NetBulletCR)
   - Broken pattern: Gun2CR (CR15NetBullet2CR)

3. **Component Hash Extraction**
   - GunCR component: 0x16bafee3754402a4 (CR15NetBulletCR)
   - Gun2CR component: 0xf4ba53ab1e5be8ae (CR15NetBullet2CR)
   - Hash database: Analyzed and searched

### ⏳ IN PROGRESS
1. **Disabled Weapon Hash Discovery**
   - Magnum (PROTOSTAR) - NOT FOUND in hash DB
   - SMG (QUARK) - NOT FOUND in hash DB
   - Chain (BLAZAR) - NOT FOUND (found generic "Chain" but not CR15 class)
   - Rifle (STARBURST) - NOT FOUND (found hand model "Rifle" but not weapon class)
   
   Status: Requires binary analysis via Ghidra to locate component definitions

### ❌ NOT STARTED
1. Multi-weapon hook extension
2. Reference value extraction (trailduration, particle IDs, flags)
3. In-game testing of all weapons
4. Ordnance/tacmod VFX fixes

## Phased Implementation Plan

### PHASE 1: Weapon Component Hash Discovery
**Goal:** Find component hashes for Magnum, SMG, Chain, Rifle

**Method 1 - Ghidra Binary Analysis (RECOMMENDED)**
```bash
# Use ghydra_script_execute with Ghidra instance to:
# 1. Search symbol table for CR15NetMagnum, CR15NetSMG, CR15NetChain, CR15NetRifle
# 2. Search .rdata for vtable patterns
# 3. Find InitBulletCI callers and trace back to weapon classes
# 4. Extract component ID hashes from component definitions
```

**Method 2 - String Searching**
```bash
strings echovr.exe | grep -E "Magnum|PROTOSTAR|SMG|QUARK|Chain|BLAZAR|Rifle|STARBURST"
```

**Method 3 - Runtime Hook**
- Fire each disabled weapon
- Hook InitBulletCI and log all component hashes seen
- Extract hashes from debug output

**Expected Output:**
```
COMPONENT_CR15NetMagnum_Bullet:  0x????????
COMPONENT_CR15NetSMG_Bullet:     0x????????
COMPONENT_CR15NetChain_Bullet:   0x????????
COMPONENT_CR15NetRifle_Bullet:   0x????????
```

**Effort:** 1-2 hours
**Owner:** RE analyst (via Ghidra)

---

### PHASE 2: Reference Value Extraction
**Goal:** Get working particle ID and flag values from GunCR

**Method:** Extend Gun2CR hook to log GunCR bullet initialization
```cpp
// In gun2cr_hook.cpp - add diagnostic logging
if (component_id.type_hash == COMPONENT_CR15NetBulletCR) {
    // Log all field values from props->SProperties
    log("GunCR Bullet Properties:");
    log("  trailduration: %f", props->trailduration);
    log("  collisionpfx: 0x%llx", props->collisionpfx);
    log("  trailpfx: 0x%llx", props->trailpfx);
    log("  trailpfx_b: 0x%llx", props->trailpfx_b);
    log("  flags: 0x%x", props->flags);
}
```

**Process:**
1. Build dbghooks.dll with logging enabled
2. Launch echovr with dbghooks injected
3. Fire GunCR weapon
4. Capture logged values
5. Store in gun2cr_config.ini

**Output:**
```ini
[GunCR_Reference]
trailduration=0.8
collisionpfx=0x????????????????????
trailpfx=0x????????????????????
trailpfx_b=0x????????????????????
flags=0x0F40
```

**Effort:** 1 hour
**Owner:** Developer with echovr access

---

### PHASE 3: Multi-Weapon Hook Extension
**Goal:** Add Magnum, SMG, Chain, Rifle patches to unified hook

**File:** `~/src/nevr-server/dbghooks/gun2cr_fix/gun2cr_hook.cpp`

**Changes:**
```cpp
// Add to component hash defines
#define COMPONENT_CR15NetMagnum_Bullet  0x????????
#define COMPONENT_CR15NetSMG_Bullet     0x????????
#define COMPONENT_CR15NetChain_Bullet   0x????????
#define COMPONENT_CR15NetRifle_Bullet   0x????????

// Extend WeaponVFXFix table
static const WeaponVFXFix WEAPON_FIXES[] = {
    // Gun2CR (EXISTING)
    {
        0x1e5be8ae,  // CR15NetBullet2CR
        0.8f,        // trailduration (from GunCR reference)
        0x????,      // collisionpfx
        0x????,      // trailpfx
        0x????,      // trailpfx_b
        0x0F40       // flags
    },
    // Magnum (NEW)
    {
        0x????????,
        0.8f,
        0x????,
        0x????,
        0x????,
        0x0F40
    },
    // SMG (NEW)
    {
        0x????????,
        0.8f,
        0x????,
        0x????,
        0x????,
        0x0F40
    },
    // Chain (NEW)
    {
        0x????????,
        0.8f,
        0x????,
        0x????,
        0x????,
        0x0F40
    },
    // Rifle (NEW)
    {
        0x????????,
        0.8f,
        0x????,
        0x????,
        0x????,
        0x0F40
    },
};

// Extend InitBulletCI hook logic
for (const WeaponVFXFix& fix : WEAPON_FIXES) {
    if (component_id.type_hash == fix.component_hash) {
        props->trailduration = fix.trailduration;
        props->collisionpfx = fix.collisionpfx;
        props->trailpfx = fix.trailpfx;
        props->trailpfx_b = fix.trailpfx_b;
        props->flags |= fix.flags;
        break;
    }
}
```

**Build:**
```bash
cd ~/src/nevr-server/dbghooks
cmake -B build && cmake --build build --config Release
```

**Output:** Updated dbghooks.dll with all weapon patches

**Effort:** 1-2 hours
**Owner:** Developer

---

### PHASE 4: In-Game Testing
**Goal:** Verify all patched weapons have visible visual effects

**Test Checklist:**

| Weapon | Test | Expected Result | Status |
|--------|------|-----------------|--------|
| Gun2CR | Fire | Bullets visible with trails | [ ] |
| Gun2CR | Fire | Muzzle flash visible | [ ] |
| Gun2CR | Fire | Impact particles on walls | [ ] |
| Magnum | Fire | Bullets visible with trails | [ ] |
| Magnum | Fire | Muzzle flash visible | [ ] |
| Magnum | Fire | Impact particles on walls | [ ] |
| SMG | Fire | Bullets visible with trails | [ ] |
| SMG | Fire | Muzzle flash visible | [ ] |
| SMG | Fire | Impact particles on walls | [ ] |
| Chain | Fire | Bullets visible with trails | [ ] |
| Chain | Fire | Muzzle flash visible | [ ] |
| Chain | Fire | Impact particles on walls | [ ] |
| Rifle | Fire | Bullets visible with trails | [ ] |
| Rifle | Fire | Muzzle flash visible | [ ] |
| Rifle | Fire | Impact particles on walls | [ ] |

**Performance Test:**
- [ ] No frame rate drops
- [ ] No stuttering or lag
- [ ] No crashes during extended firing

**Effort:** 2 hours
**Owner:** Tester with echovr access

---

### PHASE 5: Ordnance/Tacmod Investigation (OPTIONAL)
**Goal:** Fix visual effects for secondary abilities

**Affected Items:**
- Translocator (ordnance)
- DOT Field (ordnance)
- Attack Drone (ordnance)
- Power Boost (tacmod)
- Point Blank (tacmod)
- Smoke/Voxel Wall (tacmod) - partially enabled

**Investigation:**
1. Check if abilities use bullet component system or different handler
2. Review BLAZAR_PARTICLE_FIX.md for config-based approach
3. Determine if hook extension or config modification needed
4. Extract reference values for each ability type

**Effort:** 3-4 hours (if proceeding)
**Owner:** RE analyst

---

## Summary Timeline

| Phase | Task | Time | Owner | Status |
|-------|------|------|-------|--------|
| 0 | Gun2CR Patch Integration | ✅ COMPLETE | Dev | DONE |
| 1 | Weapon Hash Discovery | 1-2h | RE Analyst | ⏳ PENDING |
| 2 | Reference Value Extraction | 1h | Developer | ⏳ PENDING |
| 3 | Hook Extension | 1-2h | Developer | ⏳ PENDING |
| 4 | In-Game Testing | 2h | Tester | ⏳ PENDING |
| 5 | Ordnance/Tacmod Fixes | 3-4h | RE Analyst | ❌ OPTIONAL |

**Total Effort:** 8-11 hours (9-13 with optional Phase 5)
**Estimated Completion:** 1-2 days with full team
**Critical Path:** Phase 1 (hash discovery) blocks all subsequent phases

---

## File Structure

```
~/src/nevr-server/dbghooks/
├── gun2cr_fix/
│   ├── gun2cr_hook.cpp          ← UPDATE: Add weapon patches
│   ├── gun2cr_hook.h
│   ├── gun2cr_config.ini        ← UPDATE: Add reference values
│   ├── loader.h
│   ├── loader.cpp
│   ├── MANIFEST.yaml
│   ├── README.md
│   ├── TROUBLESHOOT.md
│   ├── CMakeLists.txt
│   └── verify.py
├── config/
│   └── autoload.yaml            ← VERIFY: Gun2CR hook registered
├── CMakeLists.txt               ← Already includes gun2cr
└── WEAPON_VFX_AUDIT.md          ← Created
```

---

## Success Criteria

✅ **Phase 0 Complete:**
- [x] Gun2CR patch integrated
- [x] Config/autoload system established
- [x] Build system updated

✅ **Phase 1 Complete:**
- [ ] Magnum component hash found
- [ ] SMG component hash found
- [ ] Chain component hash found
- [ ] Rifle component hash found

✅ **Phase 2 Complete:**
- [ ] GunCR reference values extracted
- [ ] gun2cr_config.ini updated with all values

✅ **Phase 3 Complete:**
- [ ] gun2cr_hook.cpp extended with 5 weapons
- [ ] dbghooks.dll rebuilt successfully
- [ ] All hooks compile without errors

✅ **Phase 4 Complete:**
- [ ] Gun2CR particles visible in-game
- [ ] Magnum particles visible in-game
- [ ] SMG particles visible in-game
- [ ] Chain particles visible in-game
- [ ] Rifle particles visible in-game
- [ ] No crashes or performance issues
- [ ] All weapons match GunCR visual quality

---

## Blockers & Risks

### Blocker 1: Disabled Weapon Component Hashes
**Issue:** Magnum, SMG, Chain, Rifle component hashes not in hash database
**Solution:** Use Ghidra to manually extract from binary
**Risk:** Component classes may be compiled out if weapons fully disabled
**Mitigation:** Check binary size; if disabled weapons not compiled, may need alternate approach

### Blocker 2: Reference Value Availability
**Issue:** May not be able to fire GunCR in isolated environment
**Solution:** Use game session / server access
**Risk:** Reference values may vary between game versions
**Mitigation:** Extract from known-good game version (34.4.631547.1)

### Blocker 3: Hook Compatibility
**Issue:** Hook may not work if weapon code paths differ from Gun2CR
**Solution:** Use runtime logging to verify detection and patching
**Risk:** Weapons may use different component hierarchy
**Mitigation:** Trace component initialization for each weapon independently

---

## Contingency Plans

**If Phase 1 Fails (No Hashes Found):**
1. Check if disabled weapons compiled into binary at all
2. Use runtime injection to capture component hashes
3. Fall back to config-only approach (like BLAZAR_PARTICLE_FIX)
4. Modify weapon settings JSON instead of hooking

**If Phase 4 Fails (Weapons Still Broken):**
1. Verify hook is executing (add debug logging)
2. Check if component IDs match expected hashes
3. Verify GunCR reference values are correct
4. Check if particles are disabled at higher level (UI/rendering)
5. Investigate alternative patch points in rendering pipeline

---

## Next Action

**IMMEDIATE:**
1. Retrieve component hashes for disabled weapons (Ghidra analysis)
2. Extract GunCR reference values (runtime logging)
3. Update gun2cr_hook.cpp with multi-weapon patches
4. Rebuild and test

**Contact:** @developer-with-ghidra-access for Phase 1
**Contact:** @developer-with-echovr-access for Phase 2


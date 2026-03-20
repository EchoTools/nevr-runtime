// Frida script to dump message type hashes from Echo VR
// Usage: frida -l dump_message_hashes.js -f wine -- echovr.exe
//    or: frida -l dump_message_hashes.js -p <pid>

console.log("[*] Waiting for echovr.exe to load...");

// Wait for module to load, then install hooks
function installHooks() {
  const BASE = Module.findBaseAddress("echovr.exe");
  if (!BASE) {
    console.log("[!] echovr.exe not loaded yet, retrying...");
    setTimeout(installHooks, 100);
    return;
  }

  console.log(`[*] Game base address: ${BASE}`);

  // Hook addresses (offsets from base)
  const CMATSYM_HASH = BASE.add(0x107f80);
  const SMATSYMDATA_HASHA = BASE.add(0x107fd0);
  const SNS_REGISTRY_INSERT_SORTED = BASE.add(0xF88080);

  const MATSYM_FINALIZE_SEED = ptr("0x6d451003fb4b172e");

  console.log(`[*] Installing hooks at:`);
  console.log(`    CMatSym::Hash: ${CMATSYM_HASH}`);
  console.log(`    SMatSymData::HashA: ${SMATSYMDATA_HASHA}`);
  console.log(`    sns_registry_insert_sorted: ${SNS_REGISTRY_INSERT_SORTED}`);

  // Hook CMatSym::Hash - captures message type strings
  Interceptor.attach(CMATSYM_HASH, {
    onEnter: function(args) {
      try {
        this.str = args[0].readUtf8String();
      } catch (e) {
        this.str = null;
      }
    },
    onLeave: function(retval) {
      if (this.str && this.str.length > 0) {
        console.log(`[CMATSYM] "${this.str}" -> intermediate=0x${retval.toString(16)}`);
      }
    }
  });

  // Hook SMatSymData::HashA - captures finalized hashes
  Interceptor.attach(SMATSYMDATA_HASHA, {
    onEnter: function(args) {
      this.seed = args[0];
      this.value = args[1];
    },
    onLeave: function(retval) {
      if (this.seed.equals(MATSYM_FINALIZE_SEED)) {
        console.log(`[MATSYM_FINAL] intermediate=0x${this.value.toString(16)} -> FINAL=0x${retval.toString(16)}`);
      }
    }
  });

  // Hook sns_registry_insert_sorted - THE GOLDEN HOOK
  // This is the best one - gives direct hash->name mapping
  Interceptor.attach(SNS_REGISTRY_INSERT_SORTED, {
    onEnter: function(args) {
      try {
        const hash = args[0];
        const name = args[1].readUtf8String();
        const flags = args[2];
        
        if (name && name.length > 0) {
          console.log(`[MSG_REGISTRY] 0x${hash.toString(16)} = "${name}" (flags=0x${flags.toString(16)})`);
          
          // Highlight the target hash
          if (hash.toString(16) === "59e4c5ea6e01083b") {
            console.log(`\n*** FOUND TARGET HASH: "${name}" ***\n`);
          }
        }
      } catch (e) {
        console.log(`[!] Error in sns_registry_insert_sorted hook: ${e}`);
      }
    }
  });

  console.log("[*] All hooks installed, waiting for message registration...");
}

// Start hook installation when script loads
setTimeout(installHooks, 100);

#!/usr/bin/env python3
import os
import sys
import yaml

def verify_gun2cr_integration():
    base_path = os.path.dirname(os.path.abspath(__file__))
    parent_path = os.path.dirname(base_path)
    
    checks = []
    
    # Check 1: gun2cr_hook.cpp exists
    hook_cpp = os.path.join(base_path, "gun2cr_hook.cpp")
    checks.append(("gun2cr_hook.cpp exists", os.path.isfile(hook_cpp)))
    
    # Check 2: gun2cr_hook.h exists
    hook_h = os.path.join(base_path, "gun2cr_hook.h")
    checks.append(("gun2cr_hook.h exists", os.path.isfile(hook_h)))
    
    # Check 3: MANIFEST.yaml valid
    manifest_path = os.path.join(base_path, "MANIFEST.yaml")
    if os.path.isfile(manifest_path):
        try:
            with open(manifest_path) as f:
                yaml.safe_load(f)
            checks.append(("MANIFEST.yaml valid", True))
        except Exception as e:
            checks.append(("MANIFEST.yaml valid", False))
    else:
        checks.append(("MANIFEST.yaml exists", False))
    
    # Check 4: README.md exists
    readme_path = os.path.join(base_path, "README.md")
    checks.append(("README.md exists", os.path.isfile(readme_path)))
    
    # Check 5: Loader files exist
    loader_h = os.path.join(base_path, "loader.h")
    loader_cpp = os.path.join(base_path, "loader.cpp")
    checks.append(("loader.h exists", os.path.isfile(loader_h)))
    checks.append(("loader.cpp exists", os.path.isfile(loader_cpp)))
    
    # Check 6: gun2cr_config.ini exists
    config_ini = os.path.join(base_path, "gun2cr_config.ini")
    checks.append(("gun2cr_config.ini exists", os.path.isfile(config_ini)))
    
    # Check 7: CMakeLists.txt exists
    cmake_file = os.path.join(base_path, "CMakeLists.txt")
    checks.append(("CMakeLists.txt exists", os.path.isfile(cmake_file)))
    
    # Check 8: Registered in autoload.yaml
    autoload_path = os.path.join(parent_path, "config", "autoload.yaml")
    if os.path.isfile(autoload_path):
        with open(autoload_path) as f:
            autoload = yaml.safe_load(f)
            gun2cr_registered = any(h.get('name') == 'gun2cr_visual_effects_fix' for h in autoload.get('hooks', []))
            checks.append(("Registered in autoload.yaml", gun2cr_registered))
    else:
        checks.append(("autoload.yaml exists", False))
    
    # Check 9: TROUBLESHOOT.md exists
    troubleshoot = os.path.join(base_path, "TROUBLESHOOT.md")
    checks.append(("TROUBLESHOOT.md exists", os.path.isfile(troubleshoot)))
    
    # Print results
    print("\n=== Gun2CR Integration Verification ===\n")
    passed = 0
    failed = 0
    for check_name, result in checks:
        status = "✅ PASS" if result else "❌ FAIL"
        print(f"{status}: {check_name}")
        if result:
            passed += 1
        else:
            failed += 1
    
    print(f"\nTotal: {passed} passed, {failed} failed")
    return failed == 0

if __name__ == "__main__":
    success = verify_gun2cr_integration()
    sys.exit(0 if success else 1)

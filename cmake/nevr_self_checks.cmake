# nevr_self_checks.cmake: whether a build reports its run-card checks to the game service
# (src/runtime/compat/self_check.h).
#
# The answer follows the STAMPED version (cmake/set_project_version_from_git.cmake), never an environment
# or a label: a CI build exactly on a release tag is stamped "X.Y.Z" and reports; every other build is
# stamped "X.Y.(Z+1)-dev.<N>+<sha>" and does not. The decision is made when the files are built, so it is
# the same for a release before and after its pre-release flag is unticked: promotion changes no byte.
# -DNEVR_SELF_CHECKS=ON turns the unit on for any build (a developer's own run).

# Sets <out> to ON when `version` is a release version (exactly "<x.y.z>", no pre-release part and no build
# metadata), else OFF.
function(nevr_self_checks_by_stamp out version)
  if(version MATCHES "^[0-9]+\\.[0-9]+\\.[0-9]+$")
    set(${out} ON PARENT_SCOPE)
  else()
    set(${out} OFF PARENT_SCOPE)
  endif()
endfunction()

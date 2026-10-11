# nevr_self_checks.cmake: whether a build reports its run-card checks to the game service
# (src/runtime/compat/self_check.h).
#
# The answer follows the STAMPED version (cmake/nevr_rc_label.cmake), never the raw NEVR_RC_LABEL: a local
# `just package-dev` build passes -DNEVR_RC_LABEL=dev and is stamped -dev, and a -DNEVR_RC_LABEL=rc.<N> that
# is not a CI build of the matching tag is stamped -dev too. Only a version carrying "-rc.<N>" is a release
# candidate. -DNEVR_SELF_CHECKS=ON turns the unit on for any build (a developer's own run).

# Sets <out> to ON when `version` is a release-candidate version ("<x.y.z>-rc.<N>[+...]"), else OFF.
function(nevr_self_checks_by_stamp out version)
  if(version MATCHES "-rc\\.[0-9]+")
    set(${out} ON PARENT_SCOPE)
  else()
    set(${out} OFF PARENT_SCOPE)
  endif()
endfunction()

//! nevr_install: the one core install method of nevr-runtime (docs/adr/0023-core-install-method.md).
//!
//! Steps 1 to 8 are the order of checks of `docs/reference/verifying-a-release.md` (`manifest`, `fetch`, `artifact`);
//! step 9 is the platform plan (`quest`, `pc`); the run is logged by `log` and driven by `session`. The C ABI is in `ffi`
//! and its header is `include/nevr_install.h`.

pub mod artifact;
pub mod device;
pub mod error;
pub mod fetch;
pub mod ffi;
pub mod launcher;
pub mod log;
pub mod manifest;
pub mod pc;
pub mod quest;
pub mod session;
pub mod stage;
pub mod util;

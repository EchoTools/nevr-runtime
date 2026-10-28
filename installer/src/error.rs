//! One error type for every check. `step` is the step number of the nine-step method (ADR 0023): 1 to 8 are the order of
//! checks of `docs/reference/verifying-a-release.md`, 9 is the platform plan and its execution, 10 the post-install
//! verification, 11 the first-launch check. Step 0 is a failure before any step (arguments, the log).

use std::fmt;

#[derive(Debug, Clone, PartialEq, Eq)]
pub struct Error {
    pub step: u32,
    pub msg: String,
}

impl Error {
    pub fn new(step: u32, msg: impl Into<String>) -> Self {
        Error { step, msg: msg.into() }
    }
}

impl fmt::Display for Error {
    fn fmt(&self, f: &mut fmt::Formatter<'_>) -> fmt::Result {
        write!(f, "FAIL step {}: {}", self.step, self.msg)
    }
}

impl std::error::Error for Error {}

pub type Result<T> = std::result::Result<T, Error>;

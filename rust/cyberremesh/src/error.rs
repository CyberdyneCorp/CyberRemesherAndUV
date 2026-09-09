//! What the engine refused, and why.

use cyberremesh_sys as sys;

/// A refusal from the engine.
#[derive(Debug, Clone, PartialEq, Eq)]
pub struct Error {
    /// The C ABI call that refused.
    pub operation: &'static str,
    /// The engine's own name for the status.
    pub status: String,
    /// The engine's last-error text, where it set one.
    pub detail: Option<String>,
}

impl std::fmt::Display for Error {
    fn fmt(&self, f: &mut std::fmt::Formatter<'_>) -> std::fmt::Result {
        write!(f, "{}: {}", self.operation, self.status)?;
        if let Some(detail) = &self.detail {
            write!(f, " ({detail})")?;
        }
        Ok(())
    }
}

impl std::error::Error for Error {}

impl Error {
    /// A refusal this wrapper makes before the engine is called.
    ///
    /// Kept distinct from a status the engine returned, because they are
    /// different facts: one says the library refused, the other says we never
    /// asked it. Reported through the same type so a caller has one error.
    pub(crate) fn misuse(operation: &'static str, detail: impl Into<String>) -> Self {
        Self {
            operation,
            status: "refused before the call".to_string(),
            detail: Some(detail.into()),
        }
    }
}

pub type Result<T> = std::result::Result<T, Error>;

/// Turns a status into a result, reading the engine's own strings for both
/// halves rather than restating them here — a status name duplicated in Rust
/// is a string that can disagree with the one the engine prints.
pub(crate) fn check(status: sys::CyberStatus::Type, operation: &'static str) -> Result<()> {
    if status == sys::CyberStatus::CYBER_OK {
        return Ok(());
    }
    Err(Error {
        operation,
        status: status_string(status),
        detail: last_error(),
    })
}

fn status_string(status: sys::CyberStatus::Type) -> String {
    // SAFETY: the engine returns a static NUL-terminated string for any status,
    // including one it does not recognise.
    let raw = unsafe { sys::cyber_status_string(status) };
    if raw.is_null() {
        return format!("status {status}");
    }
    // SAFETY: non-null and NUL-terminated, owned by the engine and static.
    unsafe { std::ffi::CStr::from_ptr(raw) }
        .to_string_lossy()
        .into_owned()
}

/// The engine's last-error text, where it set one.
///
/// Read only on a failure and never cached: the slot is thread-local and the
/// next call on this thread overwrites it, so a copy kept past the refusal it
/// describes would eventually name a different one.
fn last_error() -> Option<String> {
    // SAFETY: returns a NUL-terminated string or null.
    let raw = unsafe { sys::cyber_last_error() };
    if raw.is_null() {
        return None;
    }
    // SAFETY: non-null and NUL-terminated, owned by the engine.
    let text = unsafe { std::ffi::CStr::from_ptr(raw) }
        .to_string_lossy()
        .into_owned();
    (!text.is_empty()).then_some(text)
}

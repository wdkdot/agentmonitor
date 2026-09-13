use serde::Serialize;

#[derive(Debug, Clone, Serialize, PartialEq)]
#[serde(rename_all = "snake_case")]
pub enum ProviderStatus {
    Available,
    NotConfigured,
    TemporarilyUnavailable,
    AuthenticationRequired,
}

#[derive(Debug, Clone, Serialize, PartialEq)]
pub struct UsageWindow {
    /// Percentage in the range 0.0..=100.0.
    pub used_percent: f64,
    /// Absolute UTC Unix timestamp in seconds.
    pub resets_at: Option<i64>,
    /// Window size when known, in seconds.
    pub window_seconds: Option<u64>,
}

impl UsageWindow {
    pub fn new(used_percent: f64, resets_at: Option<i64>, window_seconds: Option<u64>) -> Self {
        Self {
            used_percent: used_percent.clamp(0.0, 100.0),
            resets_at,
            window_seconds,
        }
    }
}

#[derive(Debug, Clone, Serialize, PartialEq)]
pub struct ScopedUsage {
    pub label: String,
    pub used_percent: f64,
    pub resets_at: Option<i64>,
}

#[derive(Debug, Clone, Serialize, PartialEq, Default)]
pub struct UsageData {
    pub session: Option<UsageWindow>,
    pub weekly: Option<UsageWindow>,
    /// Token activity for the current local week, Monday first.
    #[serde(default, skip_serializing_if = "all_daily_values_missing")]
    pub daily: [Option<u64>; 7],
    #[serde(default, skip_serializing_if = "Vec::is_empty")]
    pub scoped: Vec<ScopedUsage>,
    #[serde(skip_serializing_if = "Option::is_none")]
    pub plan: Option<String>,
}

fn all_daily_values_missing(values: &[Option<u64>; 7]) -> bool {
    values.iter().all(Option::is_none)
}

#[derive(Debug, Clone, Serialize, PartialEq)]
pub struct ProviderSnapshot {
    pub status: ProviderStatus,
    #[serde(skip_serializing_if = "Option::is_none")]
    pub data: Option<UsageData>,
    /// Human readable diagnostic. Missing providers are intentionally represented here,
    /// not as a process-level error.
    #[serde(skip_serializing_if = "Option::is_none")]
    pub message: Option<String>,
}

impl ProviderSnapshot {
    pub fn available(data: UsageData) -> Self {
        Self {
            status: ProviderStatus::Available,
            data: Some(data),
            message: None,
        }
    }

    pub fn not_configured(message: impl Into<String>) -> Self {
        Self {
            status: ProviderStatus::NotConfigured,
            data: None,
            message: Some(message.into()),
        }
    }

    pub fn temporary(message: impl Into<String>) -> Self {
        Self {
            status: ProviderStatus::TemporarilyUnavailable,
            data: None,
            message: Some(message.into()),
        }
    }

    pub fn auth_required(message: impl Into<String>) -> Self {
        Self {
            status: ProviderStatus::AuthenticationRequired,
            data: None,
            message: Some(message.into()),
        }
    }
}

#[derive(Debug, Clone, Serialize, PartialEq)]
pub struct HostSnapshot {
    pub protocol_version: u8,
    pub generated_at: i64,
    pub codex: ProviderSnapshot,
    pub claude: ProviderSnapshot,
}

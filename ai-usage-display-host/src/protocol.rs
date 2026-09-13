use crate::model::{HostSnapshot, ProviderSnapshot, ProviderStatus, UsageWindow};
use chrono::Local;

pub const PACKET_SIZE: usize = 64;
pub const MAGIC: [u8; 4] = *b"AIUD";

/// Compact fixed packet sent as the USB HID report payload.
///
/// Layout (little-endian):
/// 0..4   magic "AIUD"
/// 4      protocol version
/// 5      flags
/// 6      Codex provider status
/// 7      Claude provider status
/// 8..16  generated_at (i64)
/// 16..22 Codex session  (permille u16, reset u32)
/// 22..28 Codex weekly   (permille u16, reset u32)
/// 28..34 Claude session (permille u16, reset u32)
/// 34..40 Claude weekly  (permille u16, reset u32)
/// 40..42 host UTC offset in minutes (i16)
/// 42..49 Codex daily token activity, Monday..Sunday (u8 relative height)
/// 49..56 Claude daily token activity, Monday..Sunday (u8 relative height)
/// 56     current local weekday (0 Monday..6 Sunday)
/// 57..64 reserved for future extension
///
/// Reset timestamps are u32 Unix seconds, sufficient until 2106. A value of 0 means unknown.
pub fn encode(snapshot: &HostSnapshot) -> [u8; PACKET_SIZE] {
    let mut out = [0u8; PACKET_SIZE];
    out[0..4].copy_from_slice(&MAGIC);
    out[4] = snapshot.protocol_version;
    out[5] = flags(snapshot);
    out[6] = status_code(&snapshot.codex.status);
    out[7] = status_code(&snapshot.claude.status);
    out[8..16].copy_from_slice(&snapshot.generated_at.to_le_bytes());
    write_window(
        &mut out[16..22],
        snapshot
            .codex
            .data
            .as_ref()
            .and_then(|d| d.session.as_ref()),
    );
    write_window(
        &mut out[22..28],
        snapshot.codex.data.as_ref().and_then(|d| d.weekly.as_ref()),
    );
    write_window(
        &mut out[28..34],
        snapshot
            .claude
            .data
            .as_ref()
            .and_then(|d| d.session.as_ref()),
    );
    write_window(
        &mut out[34..40],
        snapshot
            .claude
            .data
            .as_ref()
            .and_then(|d| d.weekly.as_ref()),
    );
    let utc_offset_minutes = (Local::now().offset().local_minus_utc() / 60) as i16;
    out[40..42].copy_from_slice(&utc_offset_minutes.to_le_bytes());
    write_daily(&mut out[42..49], &snapshot.codex);
    write_daily(&mut out[49..56], &snapshot.claude);
    out[56] = chrono::Datelike::weekday(&Local::now()).num_days_from_monday() as u8;
    if snapshot
        .codex
        .data
        .as_ref()
        .is_some_and(|d| d.daily.iter().any(Option::is_some))
        || snapshot
            .claude
            .data
            .as_ref()
            .is_some_and(|d| d.daily.iter().any(Option::is_some))
    {
        out[5] |= 1 << 4;
    }
    out
}

fn write_daily(dst: &mut [u8], provider: &ProviderSnapshot) {
    dst.fill(u8::MAX);
    if let Some(data) = provider.data.as_ref() {
        let maximum = data.daily.iter().flatten().copied().max().unwrap_or(0);
        for (output, value) in dst.iter_mut().zip(data.daily) {
            if let Some(value) = value {
                *output = if maximum == 0 {
                    0
                } else {
                    ((value.saturating_mul(100) / maximum).min(100)) as u8
                };
            }
        }
    }
}

/// Wire values intentionally start at one. A zero in bytes 6/7 identifies an
/// older v1 sender, allowing firmware to derive the coarse state from flags.
fn status_code(status: &ProviderStatus) -> u8 {
    match status {
        ProviderStatus::Available => 1,
        ProviderStatus::NotConfigured => 2,
        ProviderStatus::TemporarilyUnavailable => 3,
        ProviderStatus::AuthenticationRequired => 4,
    }
}

fn flags(s: &HostSnapshot) -> u8 {
    let mut f = 0u8;
    if matches!(s.codex.status, ProviderStatus::Available) {
        f |= 1 << 0;
    }
    if matches!(s.claude.status, ProviderStatus::Available) {
        f |= 1 << 1;
    }
    if !matches!(s.codex.status, ProviderStatus::NotConfigured) {
        f |= 1 << 2;
    }
    if !matches!(s.claude.status, ProviderStatus::NotConfigured) {
        f |= 1 << 3;
    }
    f
}

fn write_window(dst: &mut [u8], w: Option<&UsageWindow>) {
    if let Some(w) = w {
        let permille = (w.used_percent.clamp(0.0, 100.0) * 10.0).round() as u16;
        dst[0..2].copy_from_slice(&permille.to_le_bytes());
        let reset = w.resets_at.and_then(|x| u32::try_from(x).ok()).unwrap_or(0);
        dst[2..6].copy_from_slice(&reset.to_le_bytes());
    } else {
        dst[0..2].copy_from_slice(&u16::MAX.to_le_bytes());
        dst[2..6].copy_from_slice(&0u32.to_le_bytes());
    }
}

#[allow(dead_code)]
fn _assert_provider_snapshot_is_used(_: &ProviderSnapshot) {}

#[cfg(test)]
mod tests {
    use super::*;
    use crate::model::{HostSnapshot, ProviderSnapshot, UsageData, UsageWindow};

    #[test]
    fn packet_is_exactly_64_bytes_and_marks_missing_provider() {
        let snapshot = HostSnapshot {
            protocol_version: 1,
            generated_at: 1_700_000_000,
            codex: ProviderSnapshot::available(UsageData {
                session: Some(UsageWindow::new(12.3, Some(1_700_000_100), Some(18_000))),
                ..Default::default()
            }),
            claude: ProviderSnapshot::not_configured("not used"),
        };
        let p = encode(&snapshot);
        assert_eq!(p.len(), 64);
        assert_eq!(&p[0..4], b"AIUD");
        assert_eq!(p[6], 1);
        assert_eq!(p[7], 2);
        assert_eq!(u16::from_le_bytes([p[16], p[17]]), 123);
        assert_eq!(u16::from_le_bytes([p[28], p[29]]), u16::MAX);
        let utc_offset = i16::from_le_bytes([p[40], p[41]]);
        assert!((-12 * 60..=14 * 60).contains(&utc_offset));
    }

    #[test]
    fn packet_preserves_detailed_provider_status() {
        let snapshot = HostSnapshot {
            protocol_version: 1,
            generated_at: 1_700_000_000,
            codex: ProviderSnapshot::temporary("retry later"),
            claude: ProviderSnapshot::auth_required("sign in"),
        };

        let p = encode(&snapshot);
        assert_eq!(p[6], 3);
        assert_eq!(p[7], 4);
    }

    #[test]
    fn packet_normalizes_daily_token_buckets_to_peak() {
        let snapshot = HostSnapshot {
            protocol_version: 1,
            generated_at: 1_700_000_000,
            codex: ProviderSnapshot::available(UsageData {
                daily: [Some(1000), Some(500), Some(0), None, None, None, None],
                ..Default::default()
            }),
            claude: ProviderSnapshot::not_configured("not used"),
        };
        let packet = encode(&snapshot);
        assert_ne!(packet[5] & (1 << 4), 0);
        assert_eq!(&packet[42..46], &[100, 50, 0, u8::MAX]);
    }
}

pub mod model;
pub mod protocol;
pub mod provider;
pub mod usb;

use std::time::{SystemTime, UNIX_EPOCH};

use model::{HostSnapshot, ProviderSnapshot, ProviderStatus, UsageData, UsageWindow};
use provider::claude::ClaudeProvider;
use provider::codex::CodexProvider;
use provider::UsageProvider;

#[derive(Default)]
pub struct UsageCache {
    codex: Option<UsageData>,
    claude: Option<UsageData>,
}

impl UsageCache {
    pub fn apply(&mut self, snapshot: &mut HostSnapshot) {
        apply_provider_cache(&mut snapshot.codex, &mut self.codex);
        apply_provider_cache(&mut snapshot.claude, &mut self.claude);
    }
}

fn apply_provider_cache(provider: &mut ProviderSnapshot, cache: &mut Option<UsageData>) {
    match provider.status {
        ProviderStatus::Available => *cache = provider.data.clone(),
        ProviderStatus::TemporarilyUnavailable | ProviderStatus::AuthenticationRequired => {
            if provider.data.is_none() {
                provider.data = cache.clone();
                if provider.data.is_some() {
                    let suffix = "마지막 정상값을 유지 중입니다.";
                    provider.message = Some(match provider.message.take() {
                        Some(message) => format!("{message} {suffix}"),
                        None => suffix.to_string(),
                    });
                }
            }
        }
        ProviderStatus::NotConfigured => *cache = None,
    }
}

pub fn collect_snapshot() -> HostSnapshot {
    let codex_task = std::thread::spawn(|| CodexProvider.poll());
    let claude_task = std::thread::spawn(|| ClaudeProvider.poll());
    let codex = codex_task.join().unwrap_or_else(|_| {
        ProviderSnapshot::temporary("Codex provider 내부 작업이 비정상 종료되었습니다.")
    });
    let claude = claude_task.join().unwrap_or_else(|_| {
        ProviderSnapshot::temporary("Claude provider 내부 작업이 비정상 종료되었습니다.")
    });
    HostSnapshot {
        protocol_version: 1,
        generated_at: unix_now(),
        codex,
        claude,
    }
}

pub fn demo_snapshot() -> HostSnapshot {
    let now = unix_now();
    let provider = |session, weekly, reset_offset, daily| {
        ProviderSnapshot::available(UsageData {
            session: Some(UsageWindow::new(session, Some(now + reset_offset), Some(18_000))),
            weekly: Some(UsageWindow::new(weekly, Some(now + 4 * 86_400), None)),
            daily,
            ..Default::default()
        })
    };
    HostSnapshot {
        protocol_version: 1,
        generated_at: now,
        codex: provider(56.0, 9.0, 3 * 3_600 + 56 * 60, [Some(840), Some(1320), Some(690), Some(2100), Some(1560), Some(430), Some(980)]),
        claude: provider(28.0, 41.0, 1 * 3_600 + 8 * 60, [Some(460), Some(880), Some(1250), Some(720), Some(1680), Some(320), Some(610)]),
    }
}

pub fn unix_now() -> i64 {
    SystemTime::now().duration_since(UNIX_EPOCH).unwrap_or_default().as_secs() as i64
}

pub fn provider_summary(name: &str, provider: &ProviderSnapshot) -> String {
    match provider.status {
        ProviderStatus::Available => {
            let session = provider.data.as_ref().and_then(|data| data.session.as_ref()).map(|window| format!("{:.0}%", window.used_percent)).unwrap_or_else(|| "--".into());
            let weekly = provider.data.as_ref().and_then(|data| data.weekly.as_ref()).map(|window| format!("{:.0}%", window.used_percent)).unwrap_or_else(|| "--".into());
            format!("{name}: 세션 {session} · 주간 {weekly}")
        }
        ProviderStatus::NotConfigured => format!("{name}: 사용 안 함"),
        ProviderStatus::TemporarilyUnavailable => format!("{name}: 일시적으로 확인할 수 없음"),
        ProviderStatus::AuthenticationRequired => format!("{name}: 로그인 필요"),
    }
}

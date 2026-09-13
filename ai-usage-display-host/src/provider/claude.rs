use std::io::{BufRead, BufReader, Write};
use std::path::{Path, PathBuf};
use std::process::{Child, Command, Stdio};
use std::sync::mpsc;
use std::thread;
use std::time::{Duration, SystemTime, UNIX_EPOCH};

use chrono::{DateTime, Datelike, Local, NaiveDate};
use serde::Deserialize;
use serde_json::{json, Value};

use crate::model::{ProviderSnapshot, ScopedUsage, UsageData, UsageWindow};
use crate::provider::UsageProvider;

const USAGE_URL: &str = "https://api.anthropic.com/api/oauth/usage";
const OAUTH_BETA: &str = "oauth-2025-04-20";
const EXPIRY_MARGIN_MS: i64 = 5 * 60 * 1000;
const CLI_TIMEOUT: Duration = Duration::from_secs(30);

pub struct ClaudeProvider;

#[derive(Debug, Deserialize)]
struct ClaudeCredentials {
    #[serde(rename = "claudeAiOauth")]
    oauth: Option<ClaudeOauth>,
}

#[derive(Debug, Deserialize)]
struct ClaudeOauth {
    #[serde(rename = "accessToken")]
    access_token: String,
    #[serde(rename = "expiresAt")]
    expires_at: Option<i64>,
    #[serde(rename = "rateLimitTier")]
    rate_limit_tier: Option<String>,
    #[serde(rename = "subscriptionType")]
    subscription_type: Option<String>,
}

#[derive(Debug)]
struct LoadedToken {
    token: String,
    expires_at: Option<i64>,
    plan: Option<String>,
}

#[derive(Debug, Default, Deserialize)]
struct UsageResponse {
    five_hour: Option<UsageWindowResponse>,
    seven_day: Option<UsageWindowResponse>,
    limits: Option<Vec<LimitEntry>>,
    model_scoped: Option<Vec<LimitEntry>>,
    extra_usage: Option<ExtraUsage>,
}

#[derive(Debug, Deserialize)]
struct UsageWindowResponse {
    utilization: Option<f64>,
    resets_at: Option<Value>,
}

#[derive(Debug, Deserialize)]
struct LimitEntry {
    kind: Option<String>,
    percent: Option<f64>,
    utilization: Option<f64>,
    resets_at: Option<Value>,
    scope: Option<LimitScope>,
    display_name: Option<String>,
}

#[derive(Debug, Deserialize)]
struct LimitScope {
    model: Option<ModelScope>,
}

#[derive(Debug, Deserialize)]
struct ModelScope {
    display_name: Option<String>,
}

#[derive(Debug, Deserialize)]
struct ExtraUsage {
    is_enabled: Option<bool>,
    monthly_limit: Option<f64>,
    used_credits: Option<f64>,
    utilization: Option<f64>,
}

impl UsageProvider for ClaudeProvider {
    fn poll(&self) -> ProviderSnapshot {
        let token = match load_token() {
            Ok(Some(t)) => t,
            Ok(None) => {
                return ProviderSnapshot::not_configured(
                    "Claude Code 로그인 정보를 찾지 못했습니다. Claude를 사용하지 않는 PC라면 정상입니다.",
                )
            }
            Err(e) => return ProviderSnapshot::temporary(format!("Claude 인증 파일을 읽지 못했습니다: {e}")),
        };

        if is_expiring(token.expires_at) {
            return cli_fallback(token.plan.as_deref());
        }

        match fetch_usage_endpoint(&token.token, token.plan.clone()) {
            Ok(mut data) => {
                data.daily = load_local_daily_usage();
                ProviderSnapshot::available(data)
            }
            Err(ClaudeError::Auth) => cli_fallback(token.plan.as_deref()),
            Err(ClaudeError::RateLimited(retry)) => ProviderSnapshot::temporary(match retry {
                Some(sec) => format!("Claude usage API rate limit. 약 {sec}초 뒤 다시 시도하면 됩니다."),
                None => "Claude usage API rate limit. 이전 표시값을 유지하고 나중에 다시 조회하면 됩니다.".into(),
            }),
            Err(ClaudeError::Network(e)) => ProviderSnapshot::temporary(format!("Claude 네트워크 오류: {e}")),
            Err(ClaudeError::Schema(e)) => ProviderSnapshot::temporary(format!("Claude 응답 파싱 오류: {e}")),
        }
    }
}

fn credential_candidates() -> Vec<PathBuf> {
    let mut out = Vec::new();
    if let Some(dir) = std::env::var_os("CLAUDE_CONFIG_DIR").filter(|v| !v.is_empty()) {
        let p = PathBuf::from(dir);
        out.push(p.join(".credentials.json"));
        out.push(p.join("credentials.json"));
    }
    if let Some(home) = dirs::home_dir() {
        out.push(home.join(".claude").join(".credentials.json"));
        out.push(home.join(".claude").join("credentials.json"));
    }
    if cfg!(windows) {
        if let Some(appdata) = std::env::var_os("APPDATA") {
            let base = PathBuf::from(appdata).join("Claude");
            out.push(base.join(".credentials.json"));
            out.push(base.join("credentials.json"));
        }
    }
    dedup(out)
}

fn dedup(paths: Vec<PathBuf>) -> Vec<PathBuf> {
    let mut out = Vec::new();
    for p in paths {
        if !out.contains(&p) {
            out.push(p);
        }
    }
    out
}

fn load_token() -> Result<Option<LoadedToken>, String> {
    if let Some(path) = credential_candidates().into_iter().find(|p| p.exists()) {
        let text =
            std::fs::read_to_string(&path).map_err(|e| format!("{}: {e}", path.display()))?;
        return parse_credential_json(&text).map_err(|e| format!("{}: {e}", path.display()));
    }

    // Claude Code stores OAuth credentials in macOS Keychain on normal macOS installs.
    // The `security` invocation may cause the OS to ask the user for Keychain permission once.
    #[cfg(target_os = "macos")]
    {
        if let Some(text) = read_macos_keychain_credentials()? {
            return parse_credential_json(&text).map_err(|e| format!("macOS Keychain: {e}"));
        }
    }

    Ok(None)
}

fn parse_credential_json(text: &str) -> Result<Option<LoadedToken>, String> {
    let creds: ClaudeCredentials = serde_json::from_str(text).map_err(|e| e.to_string())?;
    let Some(oauth) = creds.oauth else {
        return Ok(None);
    };
    if oauth.access_token.is_empty() {
        return Ok(None);
    }
    Ok(Some(LoadedToken {
        token: oauth.access_token,
        expires_at: oauth.expires_at,
        plan: tier_label(
            oauth.rate_limit_tier.as_deref(),
            oauth.subscription_type.as_deref(),
        ),
    }))
}

#[cfg(target_os = "macos")]
fn read_macos_keychain_credentials() -> Result<Option<String>, String> {
    let output = Command::new("/usr/bin/security")
        .args([
            "find-generic-password",
            "-s",
            "Claude Code-credentials",
            "-w",
        ])
        .output()
        .map_err(|e| format!("cannot run security: {e}"))?;

    if !output.status.success() {
        return Ok(None);
    }
    let text = String::from_utf8(output.stdout).map_err(|e| e.to_string())?;
    let text = text.trim();
    if text.is_empty() {
        Ok(None)
    } else {
        Ok(Some(text.to_string()))
    }
}

fn tier_label(rate_limit_tier: Option<&str>, subscription: Option<&str>) -> Option<String> {
    if let Some(t) = rate_limit_tier {
        let t = t.to_ascii_lowercase();
        if t.contains("max_20x") {
            return Some("Max 20x".into());
        }
        if t.contains("max_5x") {
            return Some("Max 5x".into());
        }
    }
    subscription.map(|s| match s.to_ascii_lowercase().as_str() {
        "max" => "Max".to_string(),
        "pro" => "Pro".to_string(),
        "team" => "Team".to_string(),
        "enterprise" => "Enterprise".to_string(),
        other => other.to_string(),
    })
}

fn is_expiring(expires_at: Option<i64>) -> bool {
    let Some(exp) = expires_at else { return false };
    let now_ms = SystemTime::now()
        .duration_since(UNIX_EPOCH)
        .unwrap_or_default()
        .as_millis() as i64;
    now_ms.saturating_add(EXPIRY_MARGIN_MS) >= exp
}

#[derive(Debug)]
enum ClaudeError {
    Auth,
    RateLimited(Option<u64>),
    Network(String),
    Schema(String),
}

fn fetch_usage_endpoint(token: &str, plan: Option<String>) -> Result<UsageData, ClaudeError> {
    let agent = ureq::AgentBuilder::new()
        .timeout(Duration::from_secs(10))
        .build();
    let resp = match agent
        .get(USAGE_URL)
        .set("Authorization", &format!("Bearer {token}"))
        .set("anthropic-beta", OAUTH_BETA)
        .set("Accept", "application/json")
        .call()
    {
        Ok(resp) => resp,
        Err(ureq::Error::Status(401 | 403, _)) => return Err(ClaudeError::Auth),
        Err(ureq::Error::Status(429, resp)) => {
            let retry = resp.header("retry-after").and_then(|s| s.parse().ok());
            return Err(ClaudeError::RateLimited(retry));
        }
        Err(ureq::Error::Status(code, resp)) => {
            return Err(ClaudeError::Network(format!(
                "HTTP {code}: {}",
                resp.status_text()
            )))
        }
        Err(ureq::Error::Transport(e)) => return Err(ClaudeError::Network(e.to_string())),
    };

    let parsed: UsageResponse = resp
        .into_json()
        .map_err(|e| ClaudeError::Schema(format!("invalid JSON: {e}")))?;
    Ok(build_usage(&parsed, plan))
}

fn build_usage(ur: &UsageResponse, plan: Option<String>) -> UsageData {
    let mut data = UsageData {
        plan,
        ..Default::default()
    };
    data.session = ur
        .five_hour
        .as_ref()
        .and_then(|w| response_window(w, 5 * 60 * 60));
    data.weekly = ur
        .seven_day
        .as_ref()
        .and_then(|w| response_window(w, 7 * 24 * 60 * 60));

    for l in ur.limits.iter().flatten() {
        let kind = l.kind.as_deref().unwrap_or_default();
        if kind == "session" && data.session.is_none() {
            if let Some(percent) = l.percent.or(l.utilization) {
                data.session = Some(UsageWindow::new(
                    percent,
                    parse_reset(l.resets_at.as_ref()),
                    Some(5 * 60 * 60),
                ));
            }
            continue;
        }
        if kind == "weekly_all" && data.weekly.is_none() {
            if let Some(percent) = l.percent.or(l.utilization) {
                data.weekly = Some(UsageWindow::new(
                    percent,
                    parse_reset(l.resets_at.as_ref()),
                    Some(7 * 24 * 60 * 60),
                ));
            }
            continue;
        }
        if kind == "weekly_scoped" {
            let label = l
                .scope
                .as_ref()
                .and_then(|s| s.model.as_ref())
                .and_then(|m| m.display_name.clone())
                .unwrap_or_else(|| "Model".into());
            if let Some(percent) = l.percent.or(l.utilization) {
                data.scoped.push(ScopedUsage {
                    label,
                    used_percent: percent.clamp(0.0, 100.0),
                    resets_at: parse_reset(l.resets_at.as_ref()),
                });
            }
        }
    }

    // Claude CLI get_usage currently returns model_scoped[] in some versions.
    for l in ur.model_scoped.iter().flatten() {
        let Some(label) = l.display_name.clone() else {
            continue;
        };
        if data.scoped.iter().any(|x| x.label == label) {
            continue;
        }
        if let Some(percent) = l.utilization.or(l.percent) {
            data.scoped.push(ScopedUsage {
                label,
                used_percent: percent.clamp(0.0, 100.0),
                resets_at: parse_reset(l.resets_at.as_ref()),
            });
        }
    }

    if let Some(extra) = ur.extra_usage.as_ref() {
        let limit = extra.monthly_limit.unwrap_or(0.0);
        if extra.is_enabled == Some(true) && limit > 0.0 {
            let pct = extra
                .utilization
                .or_else(|| extra.used_credits.map(|c| c / limit * 100.0));
            if let Some(percent) = pct {
                data.scoped.push(ScopedUsage {
                    label: "Extra".into(),
                    used_percent: percent.clamp(0.0, 100.0),
                    resets_at: None,
                });
            }
        }
    }

    data
}

fn response_window(w: &UsageWindowResponse, seconds: u64) -> Option<UsageWindow> {
    Some(UsageWindow::new(
        w.utilization?,
        parse_reset(w.resets_at.as_ref()),
        Some(seconds),
    ))
}

fn parse_reset(v: Option<&Value>) -> Option<i64> {
    match v? {
        Value::Number(n) => {
            let x = n.as_f64()?;
            Some(if x > 1.0e12 {
                (x / 1000.0) as i64
            } else {
                x as i64
            })
        }
        Value::String(s) => s
            .parse::<i64>()
            .ok()
            .map(|x| if x > 1_000_000_000_000 { x / 1000 } else { x })
            .or_else(|| DateTime::parse_from_rfc3339(s).ok().map(|d| d.timestamp())),
        _ => None,
    }
}

fn cli_fallback(plan_hint: Option<&str>) -> ProviderSnapshot {
    match cli_get_usage() {
        Ok(mut data) => {
            if data.plan.is_none() {
                data.plan = plan_hint.map(ToOwned::to_owned);
            }
            data.daily = load_local_daily_usage();
            ProviderSnapshot::available(data)
        }
        Err(CliError::NotFound) => ProviderSnapshot::auth_required(
            "Claude 인증 토큰이 만료되었고 Claude Code CLI를 찾지 못했습니다. Claude를 사용한다면 Claude Code를 실행해 로그인해 주세요.",
        ),
        Err(e) => ProviderSnapshot::temporary(format!("Claude CLI fallback 실패: {e}")),
    }
}

fn load_local_daily_usage() -> [Option<u64>; 7] {
    let mut daily = [None; 7];
    let Some(root) = dirs::home_dir().map(|home| home.join(".claude").join("projects")) else {
        return daily;
    };
    let today = Local::now().date_naive();
    let monday = today - chrono::Duration::days(today.weekday().num_days_from_monday() as i64);
    let mut seen = HashSet::new();
    scan_usage_files(&root, monday, &mut daily, &mut seen);
    daily
}

fn scan_usage_files(
    path: &Path,
    monday: NaiveDate,
    daily: &mut [Option<u64>; 7],
    seen: &mut HashSet<String>,
) {
    let Ok(entries) = std::fs::read_dir(path) else {
        return;
    };
    for entry in entries.flatten() {
        let path = entry.path();
        if path.is_dir() {
            scan_usage_files(&path, monday, daily, seen);
        } else if path.extension().and_then(|x| x.to_str()) == Some("jsonl") {
            let Ok(file) = File::open(path) else { continue };
            for line in BufReader::new(file).lines().map_while(Result::ok) {
                parse_local_usage_line(&line, monday, daily, seen);
            }
        }
    }
}

fn parse_local_usage_line(
    line: &str,
    monday: NaiveDate,
    daily: &mut [Option<u64>; 7],
    seen: &mut HashSet<String>,
) {
    let Ok(value) = serde_json::from_str::<Value>(line) else {
        return;
    };
    let Some(usage) = value.get("message").and_then(|m| m.get("usage")) else {
        return;
    };
    let Some(timestamp) = value
        .get("timestamp")
        .and_then(Value::as_str)
        .and_then(|text| DateTime::parse_from_rfc3339(text).ok())
    else {
        return;
    };
    let date = timestamp.with_timezone(&Local).date_naive();
    let index = (date - monday).num_days();
    if !(0..7).contains(&index) {
        return;
    }

    let identity = value
        .get("requestId")
        .or_else(|| value.get("request_id"))
        .and_then(Value::as_str)
        .or_else(|| {
            value
                .get("message")
                .and_then(|message| message.get("id"))
                .and_then(Value::as_str)
        });
    if let Some(identity) = identity {
        if !seen.insert(identity.to_owned()) {
            return;
        }
    }

    let token_fields = [
        "input_tokens",
        "output_tokens",
        "cache_creation_input_tokens",
        "cache_read_input_tokens",
    ];
    let tokens = token_fields
        .iter()
        .filter_map(|field| usage.get(field).and_then(Value::as_u64))
        .fold(0u64, u64::saturating_add);
    if tokens > 0 {
        let slot = &mut daily[index as usize];
        *slot = Some(slot.unwrap_or(0).saturating_add(tokens));
    }
}

#[derive(Debug)]
enum CliError {
    NotFound,
    Spawn(String),
    Timeout,
    Protocol(String),
}

impl std::fmt::Display for CliError {
    fn fmt(&self, f: &mut std::fmt::Formatter<'_>) -> std::fmt::Result {
        match self {
            Self::NotFound => write!(f, "Claude CLI not found"),
            Self::Spawn(s) => write!(f, "spawn failed: {s}"),
            Self::Timeout => write!(f, "Claude CLI timed out"),
            Self::Protocol(s) => write!(f, "Claude CLI protocol error: {s}"),
        }
    }
}

fn cli_get_usage() -> Result<UsageData, CliError> {
    let candidates = claude_candidates();
    let mut last_err = CliError::NotFound;
    for candidate in candidates {
        match spawn_claude(&candidate) {
            Ok(child) => match communicate_claude(child) {
                Ok(v) => return Ok(v),
                Err(e) => last_err = e,
            },
            Err(CliError::NotFound) => {}
            Err(e) => last_err = e,
        }
    }
    Err(last_err)
}

fn claude_candidates() -> Vec<PathBuf> {
    let mut out = vec![PathBuf::from("claude")];
    if let Some(home) = dirs::home_dir() {
        out.push(home.join(".local").join("bin").join(if cfg!(windows) {
            "claude.exe"
        } else {
            "claude"
        }));
        if cfg!(windows) {
            out.push(home.join(".local").join("bin").join("claude.cmd"));
        }
    }
    dedup(out)
}

fn spawn_claude(path: &Path) -> Result<Child, CliError> {
    let args = [
        "-p",
        "--input-format",
        "stream-json",
        "--output-format",
        "stream-json",
        "--verbose",
    ];
    let is_cmd_script = path
        .extension()
        .and_then(|x| x.to_str())
        .is_some_and(|x| x.eq_ignore_ascii_case("cmd") || x.eq_ignore_ascii_case("bat"));
    let is_bare_name = path.parent().is_none() || path.parent() == Some(Path::new(""));
    let mut cmd = if cfg!(windows) && (is_cmd_script || is_bare_name) {
        let mut c = Command::new("cmd.exe");
        c.arg("/c").arg(path).args(args);
        c
    } else {
        let mut c = Command::new(path);
        c.args(args);
        c
    };
    cmd.current_dir(std::env::temp_dir())
        .env_remove("CLAUDECODE")
        .env_remove("CLAUDE_CODE_ENTRYPOINT")
        .stdin(Stdio::piped())
        .stdout(Stdio::piped())
        .stderr(Stdio::null());
    match cmd.spawn() {
        Ok(c) => Ok(c),
        Err(e) if e.kind() == std::io::ErrorKind::NotFound => Err(CliError::NotFound),
        Err(e) => Err(CliError::Spawn(e.to_string())),
    }
}

fn communicate_claude(mut child: Child) -> Result<UsageData, CliError> {
    let mut stdin = child
        .stdin
        .take()
        .ok_or_else(|| CliError::Spawn("stdin unavailable".into()))?;
    let stdout = child
        .stdout
        .take()
        .ok_or_else(|| CliError::Spawn("stdout unavailable".into()))?;
    let request = json!({
        "type": "control_request",
        "request_id": "ai-usage-display",
        "request": {"subtype": "get_usage"}
    });
    serde_json::to_writer(&mut stdin, &request).map_err(|e| CliError::Protocol(e.to_string()))?;
    stdin
        .write_all(b"\n")
        .map_err(|e| CliError::Spawn(e.to_string()))?;
    stdin.flush().map_err(|e| CliError::Spawn(e.to_string()))?;

    let (tx, rx) = mpsc::channel::<Value>();
    thread::spawn(move || {
        let reader = BufReader::new(stdout);
        for line in reader.lines().map_while(Result::ok) {
            if let Ok(v) = serde_json::from_str::<Value>(&line) {
                let _ = tx.send(v);
            }
        }
    });

    let deadline = std::time::Instant::now() + CLI_TIMEOUT;
    loop {
        let now = std::time::Instant::now();
        if now >= deadline {
            let _ = child.kill();
            return Err(CliError::Timeout);
        }
        let msg = rx
            .recv_timeout(deadline - now)
            .map_err(|_| CliError::Timeout)?;
        if let Some(data) = parse_cli_message(&msg)? {
            let _ = child.kill();
            return Ok(data);
        }
    }
}

fn parse_cli_message(msg: &Value) -> Result<Option<UsageData>, CliError> {
    if msg.get("type").and_then(Value::as_str) != Some("control_response") {
        return Ok(None);
    }
    let response = msg
        .get("response")
        .ok_or_else(|| CliError::Protocol("control_response missing response".into()))?;
    if response.get("request_id").and_then(Value::as_str) != Some("ai-usage-display") {
        return Ok(None);
    }
    if let Some(error) = response.get("error").and_then(Value::as_str) {
        return Err(CliError::Protocol(error.to_string()));
    }
    let body = response
        .get("response")
        .ok_or_else(|| CliError::Protocol("get_usage response body missing".into()))?;
    let available = body
        .get("rate_limits_available")
        .or_else(|| body.get("rateLimitsAvailable"))
        .and_then(Value::as_bool)
        .unwrap_or(true);
    if !available {
        return Err(CliError::Protocol(
            "this Claude login does not expose plan rate limits".into(),
        ));
    }
    let limits = body
        .get("rate_limits")
        .or_else(|| body.get("rateLimits"))
        .ok_or_else(|| CliError::Protocol("rate_limits missing".into()))?;
    let parsed: UsageResponse = serde_json::from_value(limits.clone())
        .map_err(|e| CliError::Protocol(format!("cannot parse rate_limits: {e}")))?;
    let plan = body
        .get("subscription_type")
        .or_else(|| body.get("subscriptionType"))
        .and_then(Value::as_str)
        .map(ToOwned::to_owned);
    Ok(Some(build_usage(&parsed, plan)))
}

#[cfg(test)]
mod tests {
    use super::*;
    use chrono::TimeZone;

    #[test]
    fn parses_legacy_and_dynamic_scoped_limits() {
        let parsed: UsageResponse = serde_json::from_value(json!({
            "five_hour": {"utilization": 13.0, "resets_at": "2026-09-11T15:00:00Z"},
            "seven_day": {"utilization": 44.0, "resets_at": 1789000000},
            "limits": [
                {"kind":"weekly_scoped", "percent":61.0, "scope":{"model":{"display_name":"Opus"}}}
            ]
        }))
        .unwrap();
        let data = build_usage(&parsed, Some("Max".into()));
        assert_eq!(data.session.unwrap().used_percent, 13.0);
        assert_eq!(data.weekly.unwrap().used_percent, 44.0);
        assert_eq!(data.scoped[0].label, "Opus");
        assert_eq!(data.scoped[0].used_percent, 61.0);
    }

    #[test]
    fn parses_new_limits_when_legacy_fields_absent() {
        let parsed: UsageResponse = serde_json::from_value(json!({
            "limits": [
                {"kind":"session", "percent":12.0, "resets_at":1789000000},
                {"kind":"weekly_all", "percent":32.0, "resets_at":1789500000}
            ]
        }))
        .unwrap();
        let data = build_usage(&parsed, None);
        assert_eq!(data.session.unwrap().used_percent, 12.0);
        assert_eq!(data.weekly.unwrap().used_percent, 32.0);
    }

    #[test]
    fn aggregates_and_deduplicates_local_daily_tokens() {
        let today = Local::now().date_naive();
        let monday = today - chrono::Duration::days(today.weekday().num_days_from_monday() as i64);
        let timestamp = Local
            .with_ymd_and_hms(today.year(), today.month(), today.day(), 12, 0, 0)
            .single()
            .unwrap()
            .to_rfc3339();
        let line = json!({
            "timestamp": timestamp,
            "requestId": "request-1",
            "message": {"usage": {
                "input_tokens": 100,
                "output_tokens": 20,
                "cache_read_input_tokens": 300
            }}
        })
        .to_string();
        let mut daily = [None; 7];
        let mut seen = HashSet::new();
        parse_local_usage_line(&line, monday, &mut daily, &mut seen);
        parse_local_usage_line(&line, monday, &mut daily, &mut seen);
        assert_eq!(
            daily[today.weekday().num_days_from_monday() as usize],
            Some(420)
        );
    }
}
use std::collections::HashSet;
use std::fs::File;

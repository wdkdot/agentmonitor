use std::io::{BufRead, BufReader, Write};
use std::path::PathBuf;
use std::process::{Child, Command, Stdio};
use std::sync::mpsc;
use std::thread;
use std::time::Duration;

use base64::Engine;
use chrono::{Datelike, Local, NaiveDate};
use serde::Deserialize;
use serde_json::{json, Value};

use crate::model::{ProviderSnapshot, UsageData, UsageWindow};
use crate::provider::UsageProvider;

const WHAM_URL: &str = "https://chatgpt.com/backend-api/wham/usage";
const APP_SERVER_TIMEOUT: Duration = Duration::from_secs(15);

pub struct CodexProvider;

#[derive(Debug, Deserialize, Default)]
struct CodexAuth {
    tokens: Option<CodexTokens>,
}

#[derive(Debug, Deserialize, Default)]
struct CodexTokens {
    access_token: Option<String>,
    id_token: Option<String>,
    account_id: Option<String>,
}

#[derive(Debug)]
struct Credentials {
    access_token: String,
    account_id: Option<String>,
}

impl UsageProvider for CodexProvider {
    fn poll(&self) -> ProviderSnapshot {
        let creds =
            match load_credentials() {
                Ok(Some(c)) => c,
                Ok(None) => return ProviderSnapshot::not_configured(
                    "Codex 로그인 정보를 찾지 못했습니다. Codex를 사용하지 않는 PC라면 정상입니다.",
                ),
                Err(e) => {
                    return ProviderSnapshot::temporary(format!(
                        "Codex 인증 파일을 읽지 못했습니다: {e}"
                    ))
                }
            };

        // The app-server is preferred because account/usage/read supplies the
        // authoritative daily token buckets in addition to rate-limit windows.
        match fetch_via_app_server() {
            Ok(data) => ProviderSnapshot::available(data),
            Err(app_error) => match fetch_wham(&creds) {
                Ok(data) => ProviderSnapshot::available(data),
                Err(DirectError::Auth) => ProviderSnapshot::auth_required(format!(
                    "Codex 인증이 만료되었습니다(app-server fallback: {app_error}). Codex CLI에서 다시 로그인해 주세요."
                )),
                Err(DirectError::Schema(reason)) => ProviderSnapshot::temporary(format!(
                    "Codex 사용량 응답 오류({reason}); app-server fallback: {app_error}"
                )),
                Err(DirectError::Network(e)) => {
                    ProviderSnapshot::temporary(format!("Codex 네트워크 오류: {e}"))
                }
                Err(DirectError::RateLimited) => ProviderSnapshot::temporary(
                    "Codex 사용량 조회가 일시적으로 제한되었습니다.",
                ),
            },
        }
    }
}

fn auth_path() -> Option<PathBuf> {
    if let Some(home) = std::env::var_os("CODEX_HOME") {
        if !home.is_empty() {
            return Some(PathBuf::from(home).join("auth.json"));
        }
    }
    dirs::home_dir().map(|p| p.join(".codex").join("auth.json"))
}

fn load_credentials() -> Result<Option<Credentials>, String> {
    let Some(path) = auth_path() else {
        return Ok(None);
    };
    if !path.exists() {
        return Ok(None);
    }

    let text = std::fs::read_to_string(&path).map_err(|e| format!("{}: {e}", path.display()))?;
    let auth: CodexAuth =
        serde_json::from_str(&text).map_err(|e| format!("{}: {e}", path.display()))?;
    let Some(tokens) = auth.tokens else {
        return Ok(None);
    };

    let access_token = tokens
        .access_token
        .filter(|s| !s.is_empty())
        .or_else(|| tokens.id_token.clone().filter(|s| !s.is_empty()));
    let Some(access_token) = access_token else {
        return Ok(None);
    };

    let account_id = tokens
        .account_id
        .filter(|s| !s.is_empty())
        .or_else(|| tokens.id_token.as_deref().and_then(jwt_account_id));

    Ok(Some(Credentials {
        access_token,
        account_id,
    }))
}

fn jwt_account_id(jwt: &str) -> Option<String> {
    let payload = jwt.split('.').nth(1)?;
    let bytes = base64::engine::general_purpose::URL_SAFE_NO_PAD
        .decode(payload)
        .ok()?;
    let value: Value = serde_json::from_slice(&bytes).ok()?;
    value
        .get("https://api.openai.com/auth")?
        .get("chatgpt_account_id")?
        .as_str()
        .map(ToOwned::to_owned)
}

#[derive(Debug)]
enum DirectError {
    Auth,
    RateLimited,
    Network(String),
    Schema(String),
}

fn fetch_wham(creds: &Credentials) -> Result<UsageData, DirectError> {
    let agent = ureq::AgentBuilder::new()
        .timeout(Duration::from_secs(10))
        .build();
    let mut req = agent
        .get(WHAM_URL)
        .set("Authorization", &format!("Bearer {}", creds.access_token))
        .set("User-Agent", "codex-cli")
        .set("Accept", "application/json");
    if let Some(account_id) = creds.account_id.as_deref() {
        req = req.set("ChatGPT-Account-Id", account_id);
    }

    let response = match req.call() {
        Ok(r) => r,
        Err(ureq::Error::Status(401 | 403, _)) => return Err(DirectError::Auth),
        Err(ureq::Error::Status(429, _)) => return Err(DirectError::RateLimited),
        Err(ureq::Error::Status(code, r)) => {
            return Err(DirectError::Network(format!(
                "HTTP {code}: {}",
                r.status_text()
            )))
        }
        Err(ureq::Error::Transport(e)) => return Err(DirectError::Network(e.to_string())),
    };

    let value: Value = response
        .into_json()
        .map_err(|e| DirectError::Schema(format!("invalid JSON: {e}")))?;
    parse_wham(&value)
        .ok_or_else(|| DirectError::Schema("recognized rate-limit windows not found".into()))
}

fn parse_wham(v: &Value) -> Option<UsageData> {
    let details = v.get("rate_limit").or_else(|| v.get("rateLimit"))?;
    let primary = details
        .get("primary_window")
        .or_else(|| details.get("primaryWindow"));
    let secondary = details
        .get("secondary_window")
        .or_else(|| details.get("secondaryWindow"));

    let mut windows = Vec::new();
    for w in [primary, secondary].into_iter().flatten() {
        if let Some(parsed) = parse_wham_window(w) {
            windows.push(parsed);
        }
    }
    if windows.is_empty() {
        return None;
    }
    Some(classify_windows(windows))
}

fn parse_wham_window(v: &Value) -> Option<UsageWindow> {
    let used = v
        .get("used_percent")
        .or_else(|| v.get("usedPercent"))?
        .as_f64()?;
    let resets = v
        .get("reset_at")
        .or_else(|| v.get("resets_at"))
        .or_else(|| v.get("resetsAt"))
        .and_then(value_to_i64);
    let seconds = v
        .get("limit_window_seconds")
        .or_else(|| v.get("limitWindowSeconds"))
        .and_then(value_to_u64);
    Some(UsageWindow::new(used, resets, seconds))
}

fn classify_windows(windows: Vec<UsageWindow>) -> UsageData {
    let mut data = UsageData::default();
    for window in windows {
        match window.window_seconds {
            Some(secs) if secs <= 24 * 60 * 60 => {
                if data.session.is_none() {
                    data.session = Some(window);
                }
            }
            Some(_) => {
                if data.weekly.is_none() {
                    data.weekly = Some(window);
                }
            }
            None => {
                if data.session.is_none() {
                    data.session = Some(window);
                } else if data.weekly.is_none() {
                    data.weekly = Some(window);
                }
            }
        }
    }
    data
}

#[derive(Debug)]
enum AppServerError {
    NotFound,
    Spawn(String),
    Timeout,
    Protocol(String),
}

impl std::fmt::Display for AppServerError {
    fn fmt(&self, f: &mut std::fmt::Formatter<'_>) -> std::fmt::Result {
        match self {
            Self::NotFound => write!(f, "Codex CLI not found"),
            Self::Spawn(s) => write!(f, "spawn failed: {s}"),
            Self::Timeout => write!(f, "app-server timed out"),
            Self::Protocol(s) => write!(f, "app-server protocol error: {s}"),
        }
    }
}

fn fetch_via_app_server() -> Result<UsageData, AppServerError> {
    let mut child = spawn_codex_app_server()?;
    let stdin = child
        .stdin
        .take()
        .ok_or_else(|| AppServerError::Spawn("stdin unavailable".into()))?;
    let stdout = child
        .stdout
        .take()
        .ok_or_else(|| AppServerError::Spawn("stdout unavailable".into()))?;

    let (tx, rx) = mpsc::channel::<Value>();
    thread::spawn(move || {
        let reader = BufReader::new(stdout);
        for line in reader.lines().map_while(Result::ok) {
            if let Ok(value) = serde_json::from_str::<Value>(&line) {
                let _ = tx.send(value);
            }
        }
    });

    let mut writer = stdin;
    write_json_line(
        &mut writer,
        &json!({
            "method": "initialize",
            "id": 1,
            "params": {
                "clientInfo": {
                    "name": "ai-usage-display-host",
                    "title": "AI Usage Display Host",
                    "version": env!("CARGO_PKG_VERSION")
                },
                "capabilities": {
                    "experimentalApi": false,
                    "requestAttestation": false
                }
            }
        }),
    )?;
    let init = recv_response(&rx, 1, APP_SERVER_TIMEOUT)?;
    if init.get("error").is_some() {
        let _ = child.kill();
        return Err(AppServerError::Protocol(format!(
            "initialize failed: {init}"
        )));
    }

    write_json_line(&mut writer, &json!({"method": "initialized"}))?;
    write_json_line(
        &mut writer,
        &json!({
            "method": "account/rateLimits/read",
            "id": 2,
            "params": {"excludeResetCreditDetails": true}
        }),
    )?;

    let response = recv_response(&rx, 2, APP_SERVER_TIMEOUT)?;
    let result = response
        .get("result")
        .ok_or_else(|| AppServerError::Protocol(format!("missing result: {response}")))?;
    let mut data = parse_app_server_rate_limits(result).ok_or_else(|| {
        AppServerError::Protocol("rateLimits response had no usable windows".into())
    })?;

    // Older app-server versions do not implement this method. Daily activity
    // is optional so rate-limit display remains functional with those versions.
    write_json_line(
        &mut writer,
        &json!({"method": "account/usage/read", "id": 3, "params": {}}),
    )?;
    if let Ok(usage_response) = recv_response(&rx, 3, APP_SERVER_TIMEOUT) {
        if let Some(usage) = usage_response.get("result") {
            parse_account_daily_usage(usage, &mut data);
        }
    }
    let _ = child.kill();
    Ok(data)
}

fn parse_account_daily_usage(result: &Value, data: &mut UsageData) {
    let Some(buckets) = result
        .get("dailyUsageBuckets")
        .or_else(|| result.get("daily_usage_buckets"))
        .and_then(Value::as_array)
    else {
        return;
    };
    let today = Local::now().date_naive();
    let monday = today - chrono::Duration::days(today.weekday().num_days_from_monday() as i64);
    for bucket in buckets {
        let Some(date) = bucket
            .get("startDate")
            .or_else(|| bucket.get("start_date"))
            .and_then(Value::as_str)
            .and_then(|text| NaiveDate::parse_from_str(text, "%Y-%m-%d").ok())
        else {
            continue;
        };
        let index = (date - monday).num_days();
        if (0..7).contains(&index) {
            data.daily[index as usize] = bucket.get("tokens").and_then(Value::as_u64);
        }
    }
}

fn spawn_codex_app_server() -> Result<Child, AppServerError> {
    let mut command = if cfg!(windows) {
        let mut c = Command::new("cmd.exe");
        c.arg("/c").arg("codex").arg("app-server");
        c
    } else {
        let mut c = Command::new("codex");
        c.arg("app-server");
        c
    };
    command
        .stdin(Stdio::piped())
        .stdout(Stdio::piped())
        .stderr(Stdio::null());
    match command.spawn() {
        Ok(child) => Ok(child),
        Err(e) if e.kind() == std::io::ErrorKind::NotFound => Err(AppServerError::NotFound),
        Err(e) => Err(AppServerError::Spawn(e.to_string())),
    }
}

fn write_json_line(writer: &mut impl Write, value: &Value) -> Result<(), AppServerError> {
    serde_json::to_writer(&mut *writer, value)
        .map_err(|e| AppServerError::Protocol(e.to_string()))?;
    writer
        .write_all(b"\n")
        .map_err(|e| AppServerError::Spawn(e.to_string()))?;
    writer
        .flush()
        .map_err(|e| AppServerError::Spawn(e.to_string()))
}

fn recv_response(
    rx: &mpsc::Receiver<Value>,
    id: i64,
    timeout: Duration,
) -> Result<Value, AppServerError> {
    let deadline = std::time::Instant::now() + timeout;
    loop {
        let now = std::time::Instant::now();
        if now >= deadline {
            return Err(AppServerError::Timeout);
        }
        let left = deadline - now;
        let msg = rx.recv_timeout(left).map_err(|_| AppServerError::Timeout)?;
        if msg.get("id").and_then(Value::as_i64) == Some(id) {
            return Ok(msg);
        }
    }
}

fn parse_app_server_rate_limits(result: &Value) -> Option<UsageData> {
    let snapshot = result
        .get("rateLimits")
        .or_else(|| result.get("rate_limits"))?;
    let primary = snapshot.get("primary");
    let secondary = snapshot.get("secondary");
    let mut windows = Vec::new();
    for w in [primary, secondary].into_iter().flatten() {
        if w.is_null() {
            continue;
        }
        let used = w
            .get("usedPercent")
            .or_else(|| w.get("used_percent"))
            .and_then(Value::as_f64)?;
        let resets = w
            .get("resetsAt")
            .or_else(|| w.get("resets_at"))
            .and_then(value_to_i64);
        let mins = w
            .get("windowDurationMins")
            .or_else(|| w.get("window_duration_mins"))
            .and_then(value_to_u64);
        windows.push(UsageWindow::new(used, resets, mins.map(|m| m * 60)));
    }
    if windows.is_empty() {
        None
    } else {
        Some(classify_windows(windows))
    }
}

fn value_to_i64(v: &Value) -> Option<i64> {
    v.as_i64()
        .or_else(|| v.as_u64().and_then(|x| i64::try_from(x).ok()))
        .or_else(|| v.as_str()?.parse().ok())
}

fn value_to_u64(v: &Value) -> Option<u64> {
    v.as_u64()
        .or_else(|| v.as_i64().and_then(|x| u64::try_from(x).ok()))
        .or_else(|| v.as_str()?.parse().ok())
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn parses_and_classifies_wham_windows_by_duration() {
        let v = json!({
            "rate_limit": {
                "primary_window": {"used_percent": 12.5, "limit_window_seconds": 18000, "reset_at": 123},
                "secondary_window": {"used_percent": 44.0, "limit_window_seconds": 604800, "reset_at": 456}
            }
        });
        let data = parse_wham(&v).unwrap();
        assert_eq!(data.session.unwrap().used_percent, 12.5);
        assert_eq!(data.weekly.unwrap().used_percent, 44.0);
    }

    #[test]
    fn weekly_only_is_not_misreported_as_session() {
        let v = json!({
            "rate_limit": {
                "primary_window": {"used_percent": 21, "limit_window_seconds": 604800, "reset_at": 456},
                "secondary_window": null
            }
        });
        let data = parse_wham(&v).unwrap();
        assert!(data.session.is_none());
        assert_eq!(data.weekly.unwrap().used_percent, 21.0);
    }

    #[test]
    fn parses_official_daily_usage_buckets_for_current_week() {
        let today = Local::now().date_naive();
        let monday = today - chrono::Duration::days(today.weekday().num_days_from_monday() as i64);
        let mut data = UsageData::default();
        parse_account_daily_usage(
            &json!({
                "dailyUsageBuckets": [
                    {"startDate": monday.format("%Y-%m-%d").to_string(), "tokens": 1200},
                    {"startDate": (monday + chrono::Duration::days(1)).format("%Y-%m-%d").to_string(), "tokens": 600}
                ]
            }),
            &mut data,
        );
        assert_eq!(data.daily[0], Some(1200));
        assert_eq!(data.daily[1], Some(600));
    }
}

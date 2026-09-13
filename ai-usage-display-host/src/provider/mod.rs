pub mod claude;
pub mod codex;

use crate::model::ProviderSnapshot;

pub trait UsageProvider {
    fn poll(&self) -> ProviderSnapshot;
}

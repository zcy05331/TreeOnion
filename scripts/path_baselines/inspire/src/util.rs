use std::time::Duration;

#[derive(Clone, Copy, Default, serde::Serialize)]
pub(crate) struct Summary {
    pub(crate) count: usize,
    pub(crate) total_ms: f64,
    pub(crate) mean_ms: f64,
}

pub(crate) fn ms(duration: Duration) -> f64 {
    duration.as_secs_f64() * 1000.0
}

pub(crate) fn summarize(values: impl IntoIterator<Item = f64>) -> Summary {
    let mut count = 0usize;
    let mut total_ms = 0.0;
    for value in values {
        count += 1;
        total_ms += value;
    }
    Summary {
        count,
        total_ms,
        mean_ms: if count == 0 {
            0.0
        } else {
            total_ms / count as f64
        },
    }
}

pub(crate) fn mean_usize(values: impl IntoIterator<Item = usize>) -> f64 {
    let mut count = 0usize;
    let mut total = 0usize;
    for value in values {
        count += 1;
        total += value;
    }
    if count == 0 {
        0.0
    } else {
        total as f64 / count as f64
    }
}

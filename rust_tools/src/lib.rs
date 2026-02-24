#[repr(C, align(64))]
#[derive(Debug, Copy, Clone)]
pub struct TickRecord {
    pub symbol: [u8; 32],
    pub symbol_id: u64,
    pub trading_day: u32,
    pub _pad1: u32,
    pub update_time: u64,
    pub last_price: f64,
    pub volume: i32,
    pub _pad2: u32,
    pub turnover: f64,
    pub open_interest: f64,
    pub upper_limit: f64,
    pub lower_limit: f64,
    pub open_price: f64,
    pub highest_price: f64,
    pub lowest_price: f64,
    pub pre_close_price: f64,
    pub bid_price: [f64; 5],
    pub bid_volume: [i32; 5],
    pub _pad3: u32,
    pub ask_price: [f64; 5],
    pub ask_volume: [i32; 5],
    pub _pad4: [u8; 60],
}

impl TickRecord {
    pub fn get_symbol(&self) -> String {
        String::from_utf8_lossy(&self.symbol)
            .trim_matches(char::from(0))
            .to_string()
    }
}

#[repr(C, align(64))]
#[derive(Debug, Copy, Clone)]
pub struct SnapshotSlot {
    pub seq: u32,
    pub _pad: [u8; 60],
    pub tick: TickRecord,
}

pub const MAX_SYMBOLS: usize = 2048;
pub const SYMBOL_ID_BASE: u64 = 10000000;
pub const SYMBOL_INDEX_SIZE: usize = 65536;
pub const SHM_MAGIC: u64 = 0x534E415053484F54;

#[repr(C)]
pub struct ShmLayout {
    pub magic: u64,
    pub symbol_index: [i32; SYMBOL_INDEX_SIZE],
    pub slots: [SnapshotSlot; MAX_SYMBOLS],
    pub slot_count: i32,
}

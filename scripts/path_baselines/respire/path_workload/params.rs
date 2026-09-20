use respire::pir::cuckoo_respire::CuckooRespireImpl;
use respire::pir::respire::RespireParamsExpanded;
use respire::pir::respire_harness::FactoryParams;
use respire::respire;

pub const SOURCE_COMMIT: &str = "fd20d38e9e94c0298ab21af2f7699a23729eef67";

#[derive(Clone, Copy)]
pub struct RespireShape {
    pub label: &'static str,
    pub nu1: usize,
    pub nu2: usize,
    pub d3: usize,
}

const FORMAL_H24_PARAMS: RespireParamsExpanded = {
    let mut params = FactoryParams::batch_256(49, 8, 8, 8);
    params.D3 = 64;
    params.expand().expand()
};
type FormalH24Base = respire!(FORMAL_H24_PARAMS);
pub type FormalH24 = CuckooRespireImpl<32, 49, { 1usize << 25 }, FormalH24Base>;
pub const FORMAL_H24_SHAPE: RespireShape = RespireShape {
    label: "formal_h24_1gib_batch_256",
    nu1: 8,
    nu2: 8,
    d3: 64,
};

const FORMAL_H26_PARAMS: RespireParamsExpanded = {
    let mut params = FactoryParams::batch_256(49, 8, 9, 9);
    params.D3 = 64;
    params.expand().expand()
};
type FormalH26Base = respire!(FORMAL_H26_PARAMS);
pub type FormalH26 = CuckooRespireImpl<32, 49, { 1usize << 27 }, FormalH26Base>;
pub const FORMAL_H26_SHAPE: RespireShape = RespireShape {
    label: "formal_h26_4gib_batch_256",
    nu1: 9,
    nu2: 9,
    d3: 64,
};

const SMOKE_H8_PARAMS: RespireParamsExpanded = {
    let mut params = FactoryParams::batch_256(49, 8, 1, 1);
    params.D3 = 64;
    params.expand().expand()
};
type SmokeH8Base = respire!(SMOKE_H8_PARAMS);
pub type SmokeH8 = CuckooRespireImpl<32, 49, { 1usize << 9 }, SmokeH8Base>;
pub const SMOKE_H8_SHAPE: RespireShape = RespireShape {
    label: "smoke_h8_batch_256",
    nu1: 1,
    nu2: 1,
    d3: 64,
};

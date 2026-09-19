use respire::pir::cuckoo_respire::CuckooRespireImpl;
use respire::pir::respire::RespireParamsExpanded;
use respire::pir::respire_harness::FactoryParams;
use respire::respire;

pub const SOURCE_COMMIT: &str = "fd20d38e9e94c0298ab21af2f7699a23729eef67";

const FORMAL_H24_PARAMS: RespireParamsExpanded = {
    let mut params = FactoryParams::batch_256(49, 8, 8, 8);
    params.D3 = 64;
    params.expand().expand()
};
type FormalH24Base = respire!(FORMAL_H24_PARAMS);
pub type FormalH24 = CuckooRespireImpl<32, 49, { 1usize << 25 }, FormalH24Base>;

const SMOKE_H8_PARAMS: RespireParamsExpanded = {
    let mut params = FactoryParams::batch_256(49, 8, 1, 1);
    params.D3 = 64;
    params.expand().expand()
};
type SmokeH8Base = respire!(SMOKE_H8_PARAMS);
pub type SmokeH8 = CuckooRespireImpl<32, 49, { 1usize << 9 }, SmokeH8Base>;

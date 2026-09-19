use poulpy_pir::payload::U256P65535;

#[cfg(feature = "avx512-fhe")]
pub(crate) type BE = poulpy_cpu_avx512::FFT64Avx512;
#[cfg(all(not(feature = "avx512-fhe"), feature = "avx2-fhe"))]
pub(crate) type BE = poulpy_cpu_avx::FFT64Avx;
#[cfg(all(not(feature = "avx512-fhe"), not(feature = "avx2-fhe")))]
pub(crate) type BE = poulpy_cpu_ref::FFT64Ref;

pub(crate) type P = U256P65535;

pub(crate) fn backend_name() -> &'static str {
    #[cfg(feature = "avx512-fhe")]
    {
        "poulpy_cpu_avx512::FFT64Avx512"
    }
    #[cfg(all(not(feature = "avx512-fhe"), feature = "avx2-fhe"))]
    {
        "poulpy_cpu_avx::FFT64Avx"
    }
    #[cfg(all(not(feature = "avx512-fhe"), not(feature = "avx2-fhe")))]
    {
        "poulpy_cpu_ref::FFT64Ref"
    }
}

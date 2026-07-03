# codec_bench — the S0.3 measurement behind ADR-0017: encode representative frames with each codec
# (raw / LZ4 / JPEG) and report ratio, throughput, wire bandwidth, and JPEG PSNR. GPU-free (it makes
# frames on the CPU), so it runs on the headless streaming test bed. Links rime::stream (the codec)
# and fmt (table output; found at the root like every dependency).
add_executable(codec_bench main.cpp)

target_link_libraries(codec_bench
    PRIVATE rime::stream fmt::fmt)

rime_apply_common_target_options(codec_bench)

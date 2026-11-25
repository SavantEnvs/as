#!/usr/bin/env bash
#
# mayhem/build.sh — build the as (SSAS) fuzz harnesses and the known-answer self test.
#
# Upstream builds with SCons plus Python config generators that download packages at build
# time; the code we fuzz needs neither: the srec/ihex loader and the CanIf/CanTp/J1939Tp/PduR
# stack are plain C, and the CAN stack is linked with upstream's checked-in isotp tool
# configuration (tools/libraries/isotp/config). So the sources are compiled directly with the
# base image's compiler and flags. Outputs go to mayhem/out (gitignored by mayhem/.gitignore)
# and the fuzz binaries are installed at /mayhem/<target>.
set -euo pipefail
[ -n "${SOURCE_DATE_EPOCH:-}" ] || unset SOURCE_DATE_EPOCH

: "${SANITIZER_FLAGS=-fsanitize=address,undefined -fno-sanitize-recover=all -fno-omit-frame-pointer}"
: "${DEBUG_FLAGS:=-g -gdwarf-3}"
: "${CC:=clang}" ; : "${CXX:=clang++}" ; : "${LIB_FUZZING_ENGINE:=-fsanitize=fuzzer}"
: "${MAYHEM_JOBS:=$(nproc)}"
: "${COVERAGE_FLAGS=}"
: "${SRC:=/mayhem}"
: "${STANDALONE_FUZZ_MAIN:?STANDALONE_FUZZ_MAIN must point at the run-once driver (set by the base image)}"

cd "$SRC"
OUT="$SRC/mayhem/out"
mkdir -p "$OUT"

INC=(-Iinfras/include -Iinfras/communication/CanIf -Iinfras/communication/CanTp
     -Iinfras/communication/J1939Tp -Iinfras/communication/PduR -Itools/libraries/isotp/config
     -Iinfras/libraries/mempool -Iinfras/libraries/crc/src -Itools/libraries/srec)
DEFS=(-DUSE_CANIF -DUSE_CANTP -DUSE_J1939TP -DUSE_PDUR -DUSE_DET)

SREC_SRCS=(tools/libraries/srec/srec.c tools/libraries/srec/ihex.c
           infras/libraries/crc/src/Crc_16.c infras/libraries/crc/src/Crc_32.c)
CAN_SRCS=(infras/communication/CanIf/CanIf.c infras/communication/CanTp/CanTp.c
          infras/communication/J1939Tp/J1939Tp.c infras/communication/PduR/PduR.c
          infras/communication/PduR/PduR_CanTp.c infras/communication/PduR/PduR_J1939Tp.c
          infras/communication/PduR/PduR_CanIf.c
          tools/libraries/isotp/config/CanIf_Cfg.c tools/libraries/isotp/config/CanTp_Cfg.c
          tools/libraries/isotp/config/J1939Tp_Cfg.c tools/libraries/isotp/config/PduR_Cfg.c)

# The LIN and DoIP targets use the bootloader's LinIf/LinTp/SoAd/DoIP/NetMem configuration,
# generated (as upstream's SCons build does) by upstream's generators from app/bootloader/config.
python3 -I mayhem/gen_cfg.py "$SRC" "$OUT/gen" >/dev/null
LIN_INC=(-Iinfras/include -Iinfras/communication/LinIf -Iinfras/communication/LinTp
         -Iinfras/communication/CanTp -I"$OUT/gen/LinIf/GEN" -I"$OUT/gen/LinTp/GEN"
         -Imayhem/harnesses/lin)
LIN_DEFS=(-DUSE_LINIF -DUSE_LINTP -DUSE_DET)
LIN_SRCS=(infras/communication/LinIf/LinIf.c infras/communication/LinTp/LinTp.c
          "$OUT/gen/LinIf/GEN/LinIf_Cfg.c" "$OUT/gen/LinTp/GEN/LinTp_Cfg.c")
NET_INC=(-Iinfras/include -Iinfras/communication/SoAd -Iinfras/communication/DoIP
         -Iinfras/libraries/mempool -I"$OUT/gen/Net/GEN" -I"$OUT/gen/NetMem/GEN"
         -Imayhem/harnesses/doip)
NET_DEFS=(-DUSE_SOAD -DUSE_DOIP -DUSE_DET)
NET_SRCS=(infras/communication/SoAd/SoAd.c infras/communication/DoIP/DoIP.c
          "$OUT/gen/Net/GEN/SoAd_Cfg.c" "$OUT/gen/Net/GEN/DoIP_Cfg.c" "$OUT/gen/NetMem/GEN/NetMem.c")
SOMEIP_INC=(-Iinfras/include -Iinfras/communication/SoAd -Iinfras/communication/SomeIp
            -Iinfras/communication/Sd -Iinfras/communication/SomeIpXf -Iinfras/libraries/mempool
            -I"$OUT/gen/SomeIpAdd/GEN")
SOMEIP_DEFS=(-DUSE_SOAD -DUSE_SOMEIP -DUSE_SD -DUSE_SOMEIPXF -DUSE_DET)
SOMEIP_SRCS=(infras/communication/SoAd/SoAd.c infras/communication/SomeIp/SomeIp.c
             infras/communication/Sd/Sd.c infras/communication/SomeIpXf/SomeIpXf.c
             "$OUT/gen/SomeIpAdd/GEN/SoAd_Cfg.c" "$OUT/gen/SomeIpAdd/GEN/Sd_Cfg.c"
             "$OUT/gen/SomeIpAdd/GEN/SomeIp_Cfg.c" "$OUT/gen/SomeIpAdd/GEN/SomeIpXf_Cfg.c"
             "$OUT/gen/SomeIpAdd/GEN/MathSkeleton.c" "$OUT/gen/SomeIpAdd/GEN/NetMem.c")

DCM_INC=(-Iinfras/include -Iinfras/diagnostic/Dcm -Iinfras/communication/CanTp
         -Iinfras/communication/PduR -I"$OUT/gen/BLDcm/GEN" -I"$OUT/gen/BLCanTp/GEN"
         -I"$OUT/gen/BLPduR/GEN")
DCM_DEFS=(-DUSE_DCM -DUSE_CANTP -DUSE_PDUR -DUSE_DET)
DCM_SRCS=(infras/diagnostic/Dcm/Dcm.c infras/diagnostic/Dcm/Dcm_Dsd.c
          infras/diagnostic/Dcm/Dcm_Dsl.c infras/diagnostic/Dcm/Dcm_Dsp.c
          infras/communication/CanTp/CanTp.c infras/communication/PduR/PduR.c
          infras/communication/PduR/PduR_CanTp.c infras/communication/PduR/PduR_Dcm.c
          "$OUT/gen/BLDcm/GEN/Dcm_Cfg.c" "$OUT/gen/BLCanTp/GEN/CanTp_Cfg.c"
          "$OUT/gen/BLPduR/GEN/PduR_Cfg.c")

XCP_INC=(-Iinfras/include -Iinfras/communication/Xcp -Iinfras/libraries/mempool
         -I"$OUT/gen/Xcp/GEN" -Imayhem/harnesses/xcp)
XCP_DEFS=(-DUSE_XCP -DUSE_DET)
XCP_SRCS=(infras/communication/Xcp/Xcp.c infras/communication/Xcp/Xcp_DspStd.c
          infras/communication/Xcp/Xcp_DspCal.c infras/communication/Xcp/Xcp_DspPgm.c
          infras/communication/Xcp/Xcp_DspDaq.c infras/communication/Xcp/Xcp_Mta.c
          "$OUT/gen/Xcp/GEN/Xcp_Cfg.c")
CANNM_INC=(-Iinfras/include -Iinfras/communication/CanNm -I"$OUT/gen/CanNm/GEN")
CANNM_DEFS=(-DUSE_CANNM -DUSE_DET)
CANNM_SRCS=(infras/communication/CanNm/CanNm.c "$OUT/gen/CanNm/GEN/CanNm_Cfg.c")

# The ODX reader links tinyxml2, pre-built by mayhem/Dockerfile (uninstrumented dependency).
TINYXML2="${TINYXML2:-/opt/toolchains/deps/tinyxml2-9.0.0}"
ODX_INC=(-Itools/libraries/odx -I"$TINYXML2/include")

# compile <objdir> <flags...> -- <sources...>   (one object per source, in parallel)
compile() {
  local dir="$1"; shift
  local flags=()
  while [ "$1" != "--" ]; do flags+=("$1"); shift; done
  shift
  mkdir -p "$dir"
  printf '%s\n' "$@" | xargs -P "$MAYHEM_JOBS" -I{} sh -c \
    'f="$1"; shift; o="$0/$(echo "$f" | tr / _).o"; exec "$@" -c "$f" -o "$o"' \
    "$dir" {} "$CC" "${flags[@]}"
  objs=()
  local f
  for f in "$@"; do objs+=("$dir/$(echo "$f" | tr / _).o"); done
}

# 1) the project sources, instrumented (ASan+UBSan, coverage) for the fuzzers
FUZZ_CFLAGS=($SANITIZER_FLAGS $DEBUG_FLAGS -fsanitize=fuzzer-no-link "${INC[@]}" "${DEFS[@]}")
compile "$OUT/fuzz" "${FUZZ_CFLAGS[@]}" -- "${SREC_SRCS[@]}" "${CAN_SRCS[@]}"
fuzz_objs=("${objs[@]}")
SREC_OBJS=("${fuzz_objs[@]:0:${#SREC_SRCS[@]}}")
CAN_OBJS=("${fuzz_objs[@]:${#SREC_SRCS[@]}}")

$CXX $SANITIZER_FLAGS $DEBUG_FLAGS -c mayhem/lsan_off.cc -o "$OUT/lsan_off.o"
$CC $SANITIZER_FLAGS $DEBUG_FLAGS -c "$STANDALONE_FUZZ_MAIN" -o "$OUT/standalone_main.o"

LIN_CFLAGS=($SANITIZER_FLAGS $DEBUG_FLAGS -fsanitize=fuzzer-no-link "${LIN_INC[@]}" "${LIN_DEFS[@]}")
compile "$OUT/fuzz-lin" "${LIN_CFLAGS[@]}" -- "${LIN_SRCS[@]}"
LIN_OBJS=("${objs[@]}")

NET_CFLAGS=($SANITIZER_FLAGS $DEBUG_FLAGS -fsanitize=fuzzer-no-link "${NET_INC[@]}" "${NET_DEFS[@]}")
compile "$OUT/fuzz-net" "${NET_CFLAGS[@]}" -- "${NET_SRCS[@]}"
NET_OBJS=("${objs[@]}")
# The generated NetMem pools have 1420-byte slots (bootloader and SOME/IP example alike), so mempool.c's free-list links are misaligned
# on a 64-bit host (UBSan "misaligned address" on the first allocation of every input). Only
# the alignment check is relaxed, and only for mempool.c; ASan and the rest of UBSan stay on.
compile "$OUT/fuzz-net" "${NET_CFLAGS[@]}" -fno-sanitize=alignment -- infras/libraries/mempool/mempool.c
NET_OBJS+=("${objs[@]}")

SOMEIP_CFLAGS=($SANITIZER_FLAGS $DEBUG_FLAGS -fsanitize=fuzzer-no-link "${SOMEIP_INC[@]}" "${SOMEIP_DEFS[@]}")
compile "$OUT/fuzz-someip" "${SOMEIP_CFLAGS[@]}" -- "${SOMEIP_SRCS[@]}"
SOMEIP_OBJS=("${objs[@]}")
compile "$OUT/fuzz-someip" "${SOMEIP_CFLAGS[@]}" -fno-sanitize=alignment -- infras/libraries/mempool/mempool.c
SOMEIP_OBJS+=("${objs[@]}")

XCP_CFLAGS=($SANITIZER_FLAGS $DEBUG_FLAGS -fsanitize=fuzzer-no-link "${XCP_INC[@]}" "${XCP_DEFS[@]}")
compile "$OUT/fuzz-xcp" "${XCP_CFLAGS[@]}" -- "${XCP_SRCS[@]}"
XCP_OBJS=("${objs[@]}")
compile "$OUT/fuzz-xcp" "${XCP_CFLAGS[@]}" -fno-sanitize=alignment -- infras/libraries/mempool/mempool.c
XCP_OBJS+=("${objs[@]}")

CANNM_CFLAGS=($SANITIZER_FLAGS $DEBUG_FLAGS -fsanitize=fuzzer-no-link "${CANNM_INC[@]}" "${CANNM_DEFS[@]}")
compile "$OUT/fuzz-cannm" "${CANNM_CFLAGS[@]}" -- "${CANNM_SRCS[@]}"
CANNM_OBJS=("${objs[@]}")

DCM_CFLAGS=($SANITIZER_FLAGS $DEBUG_FLAGS -fsanitize=fuzzer-no-link "${DCM_INC[@]}" "${DCM_DEFS[@]}")
compile "$OUT/fuzz-dcm" "${DCM_CFLAGS[@]}" -- "${DCM_SRCS[@]}"
DCM_OBJS=("${objs[@]}")

$CXX $SANITIZER_FLAGS $DEBUG_FLAGS -fsanitize=fuzzer-no-link "${ODX_INC[@]}" \
  -c tools/libraries/odx/odx.cpp -o "$OUT/odx.o"

# 2) each harness twice: libFuzzer binary + run-once standalone reproducer
build_harness() {
  local name="$1"; shift
  if [ -f "mayhem/harnesses/$name.cpp" ]; then
    $CXX "${HARNESS_CFLAGS[@]}" -c "mayhem/harnesses/$name.cpp" -o "$OUT/$name.o"
  else
    $CC "${HARNESS_CFLAGS[@]}" -c "mayhem/harnesses/$name.c" -o "$OUT/$name.o"
  fi
  $CXX $SANITIZER_FLAGS $DEBUG_FLAGS $LIB_FUZZING_ENGINE "$OUT/$name.o" "$@" "$OUT/lsan_off.o" \
    -o "$SRC/$name"
  $CXX $SANITIZER_FLAGS $DEBUG_FLAGS "$OUT/standalone_main.o" "$OUT/$name.o" "$@" "$OUT/lsan_off.o" \
    -o "$SRC/$name-standalone"
}
HARNESS_CFLAGS=("${FUZZ_CFLAGS[@]}")
build_harness srec_fuzz "${SREC_OBJS[@]}"
build_harness canif_rx_fuzz "${CAN_OBJS[@]}"
HARNESS_CFLAGS=("${LIN_CFLAGS[@]}")
build_harness linif_slave_fuzz "${LIN_OBJS[@]}"
HARNESS_CFLAGS=("${NET_CFLAGS[@]}")
build_harness doip_soad_fuzz "${NET_OBJS[@]}"
HARNESS_CFLAGS=("${SOMEIP_CFLAGS[@]}")
build_harness someip_sd_fuzz "${SOMEIP_OBJS[@]}"
HARNESS_CFLAGS=("${DCM_CFLAGS[@]}")
build_harness dcm_cantp_fuzz "${DCM_OBJS[@]}"
HARNESS_CFLAGS=("${XCP_CFLAGS[@]}")
build_harness xcp_can_fuzz "${XCP_OBJS[@]}"
HARNESS_CFLAGS=("${CANNM_CFLAGS[@]}")
build_harness cannm_rx_fuzz "${CANNM_OBJS[@]}"
HARNESS_CFLAGS=($SANITIZER_FLAGS $DEBUG_FLAGS -fsanitize=fuzzer-no-link "${ODX_INC[@]}")
build_harness odx_fuzz "$OUT/odx.o" "$TINYXML2/lib/libtinyxml2.a"

# 3) the known-answer self test, with normal flags (no sanitizers) for mayhem/test.sh
TEST_CFLAGS=(-O1 $COVERAGE_FLAGS "${INC[@]}" "${DEFS[@]}")
compile "$OUT/test" "${TEST_CFLAGS[@]}" -- "${SREC_SRCS[@]}" "${CAN_SRCS[@]}"
$CC "${TEST_CFLAGS[@]}" mayhem/tests/selftest.c "${objs[@]}" $COVERAGE_FLAGS -o "$OUT/selftest"
TEST_LIN_CFLAGS=(-O1 $COVERAGE_FLAGS "${LIN_INC[@]}" "${LIN_DEFS[@]}")
compile "$OUT/test-lin" "${TEST_LIN_CFLAGS[@]}" -- "${LIN_SRCS[@]}"
$CC "${TEST_LIN_CFLAGS[@]}" mayhem/tests/selftest_lin.c "${objs[@]}" $COVERAGE_FLAGS -o "$OUT/selftest_lin"
TEST_NET_CFLAGS=(-O1 $COVERAGE_FLAGS "${NET_INC[@]}" "${NET_DEFS[@]}")
compile "$OUT/test-net" "${TEST_NET_CFLAGS[@]}" -- "${NET_SRCS[@]}" infras/libraries/mempool/mempool.c
$CC "${TEST_NET_CFLAGS[@]}" mayhem/tests/selftest_doip.c "${objs[@]}" $COVERAGE_FLAGS -o "$OUT/selftest_doip"

TEST_SOMEIP_CFLAGS=(-O1 $COVERAGE_FLAGS "${SOMEIP_INC[@]}" "${SOMEIP_DEFS[@]}")
compile "$OUT/test-someip" "${TEST_SOMEIP_CFLAGS[@]}" -- "${SOMEIP_SRCS[@]}" infras/libraries/mempool/mempool.c
$CC "${TEST_SOMEIP_CFLAGS[@]}" mayhem/tests/selftest_someip.c "${objs[@]}" $COVERAGE_FLAGS -o "$OUT/selftest_someip"
TEST_DCM_CFLAGS=(-O1 $COVERAGE_FLAGS "${DCM_INC[@]}" "${DCM_DEFS[@]}")
compile "$OUT/test-dcm" "${TEST_DCM_CFLAGS[@]}" -- "${DCM_SRCS[@]}"
$CC "${TEST_DCM_CFLAGS[@]}" mayhem/tests/selftest_dcm.c "${objs[@]}" $COVERAGE_FLAGS -o "$OUT/selftest_dcm"
TEST_XCP_CFLAGS=(-O1 $COVERAGE_FLAGS "${XCP_INC[@]}" "${XCP_DEFS[@]}")
compile "$OUT/test-xcp" "${TEST_XCP_CFLAGS[@]}" -- "${XCP_SRCS[@]}" infras/libraries/mempool/mempool.c
$CC "${TEST_XCP_CFLAGS[@]}" mayhem/tests/selftest_xcp.c "${objs[@]}" $COVERAGE_FLAGS -o "$OUT/selftest_xcp"
TEST_CANNM_CFLAGS=(-O1 $COVERAGE_FLAGS "${CANNM_INC[@]}" "${CANNM_DEFS[@]}")
compile "$OUT/test-cannm" "${TEST_CANNM_CFLAGS[@]}" -- "${CANNM_SRCS[@]}"
$CC "${TEST_CANNM_CFLAGS[@]}" mayhem/tests/selftest_cannm.c "${objs[@]}" $COVERAGE_FLAGS -o "$OUT/selftest_cannm"
$CXX -O1 $COVERAGE_FLAGS "${ODX_INC[@]}" mayhem/tests/selftest_odx.cpp tools/libraries/odx/odx.cpp \
  "$TINYXML2/lib/libtinyxml2.a" $COVERAGE_FLAGS -o "$OUT/selftest_odx"

echo "built: $SRC/{srec_fuzz,canif_rx_fuzz,linif_slave_fuzz,doip_soad_fuzz,someip_sd_fuzz,dcm_cantp_fuzz,xcp_can_fuzz,cannm_rx_fuzz,odx_fuzz} (+ -standalone), $OUT/selftest{,_lin,_doip,_someip,_dcm,_xcp,_cannm,_odx}"

#!/bin/sh
# Host protocol and launch-boundary checks. All generated files stay in native/build.
set -eu
HERE="$(cd "$(dirname "$0")" && pwd)"
OUT="$HERE/build/check"
mkdir -p "$OUT"
python3 "$HERE/test/route_check.py"
clang -std=c99 -ffreestanding -fno-builtin -Wall -Wextra -Werror \
    -I"$HERE/include" -Dopen=mcn_test_open -Dclose=mcn_test_close \
    -Dread=mcn_test_read -Dwrite=mcn_test_write -Dunlink=mcn_test_unlink \
    -Dgetenv=mcn_test_getenv -Dlseek=mcn_test_lseek \
    -c "$HERE/src/sal/iap2.c" -o "$OUT/iap2.o"
clang -std=c99 -ffreestanding -fno-builtin -Wall -Wextra -Werror \
    -I"$HERE/include" -c "$HERE/test/launch_check.c" -o "$OUT/launch.o"
clang -std=c99 -Wall -Wextra -Werror -DMCN_HOST -I"$HERE/include" \
    -c "$HERE/src/common/route.c" -o "$OUT/route.o"
clang "$OUT/iap2.o" "$OUT/launch.o" "$OUT/route.o" -o "$OUT/launch-check"
"$OUT/launch-check"
# The QNX pointer ABI is 32-bit. These host checks stop before dereferencing any
# firmware pointer; only the process-name gate and absent-driver path run here.
clang -std=c99 -ffreestanding -fno-builtin -Wall -Wextra -Werror \
    -Wno-pointer-to-int-cast -Wno-int-to-pointer-cast -I"$HERE/include" \
    -Ddlopen=mcn_test_dlopen -Ddlsym=mcn_test_dlsym \
    -c "$HERE/src/ipod/ipod.c" -o "$OUT/ipod.o"
clang -std=c99 -ffreestanding -fno-builtin -Wall -Wextra -Werror \
    -I"$HERE/include" -c "$HERE/test/ipod_check.c" -o "$OUT/ipod-check.o"
clang "$OUT/ipod.o" "$OUT/ipod-check.o" "$OUT/route.o" -o "$OUT/ipod-check"
"$OUT/ipod-check"
# Kill switch, enabled and trial boot of the actual common/hook.c.
clang -std=c99 -ffreestanding -fno-builtin -Wall -Wextra -Werror -I"$HERE/include" \
    -Dopen=mcn_test_open -Dclose=mcn_test_close -Dunlink=mcn_test_unlink \
    -Dclock_gettime=mcn_test_clock_gettime -c "$HERE/src/common/hook.c" -o "$OUT/hook.o"
clang -std=c99 -ffreestanding -fno-builtin -Wall -Wextra -Werror -I"$HERE/include" \
    -Dclock_gettime=mcn_test_clock_gettime -c "$HERE/test/hook_check.c" -o "$OUT/hook-check.o"
clang "$OUT/hook.o" "$OUT/hook-check.o" -o "$OUT/hook-check"
"$OUT/hook-check"
# Android Auto wire formats (discovery rewrite, media messages, navigation, SPS cropping).
clang -std=c99 -Wall -Wextra -Werror -DMCN_HOST -D_GNU_SOURCE -I"$HERE/include" \
    -c "$HERE/src/sal/aa_proto.c" -o "$OUT/aa_proto.o"
clang -std=c99 -Wall -Wextra -Werror -DMCN_HOST -D_GNU_SOURCE -I"$HERE/include" \
    "$HERE/test/aa_check.c" "$OUT/aa_proto.o" -o "$OUT/aa-check"
"$OUT/aa-check"
sh "$HERE/test/aa_sps_check.sh"
# The Android Auto hooks themselves (src/sal/aa.c) around a stand-in receiver library laid out
# as the P0478T one. 32-bit like the unit (pointers are kept in 32-bit words): needs the host's
# 32-bit C library (Linux: gcc-multilib), otherwise skipped.
if echo 'int main(void){return 0;}' | clang -m32 -x c - -o "$OUT/m32" 2> /dev/null; then
    AA="-m32 -std=c99 -ffreestanding -fno-builtin -Wall -Wextra -Werror -I$HERE/include
        -Ddlopen=mcn_test_dlopen -Ddlsym=mcn_test_dlsym -Dopen=mcn_test_open
        -Dclose=mcn_test_close -Dread=mcn_test_read -Dclock_gettime=mcn_test_clock_gettime"
    clang $AA -c "$HERE/src/sal/aa.c" -o "$OUT/aa-hook.o"
    clang $AA -c "$HERE/src/sal/aa_proto.c" -o "$OUT/aa-hook-proto.o"
    clang $AA -c "$HERE/test/aa_hook_check.c" -o "$OUT/aa-hook-check.o"
    clang -m32 "$OUT/aa-hook.o" "$OUT/aa-hook-proto.o" "$OUT/aa-hook-check.o" -o "$OUT/aa-hook-check"
    "$OUT/aa-hook-check"
    # The decoder module (src/sal/decode.c) and the lab player's decode path (src/sal/lab.c)
    # against stand-ins for mm-renderer, libstrm, Screen and the QNX kernel calls: the set-up
    # sequence and URLs of the notes, every mdev message byte for byte, the recording end to
    # end, queue overflow, every failing call, teardown order, a missing symbol (own process).
    DC="-m32 -std=c99 -ffreestanding -fno-builtin -Wall -Wextra -Werror -I$HERE/include
        -Ddlopen=mcn_test_dlopen -Ddlsym=mcn_test_dlsym -Dopen=mcn_test_open
        -Dclose=mcn_test_close -Dread=mcn_test_read -Dclock_gettime=mcn_test_clock_gettime
        -Dusleep=mcn_test_usleep -Dpthread_create=mcn_test_pthread_create
        -Dpthread_detach=mcn_test_pthread_detach"
    clang $DC -c "$HERE/src/sal/decode.c" -o "$OUT/decode.o"
    clang $DC -Dlseek=mcn_test_lseek -Dmmap=mcn_test_mmap -c "$HERE/src/sal/lab.c" -o "$OUT/decode-lab.o"
    clang -m32 -std=c99 -ffreestanding -fno-builtin -Wall -Wextra -Werror -I"$HERE/include" \
        -Dclock_gettime=mcn_test_clock_gettime -c "$HERE/test/decode_check.c" -o "$OUT/decode-check.o"
    clang -m32 "$OUT/decode.o" "$OUT/decode-lab.o" "$OUT/aa-hook-proto.o" "$OUT/decode-check.o" \
        -o "$OUT/decode-check"
    "$OUT/decode-check" missing
    for k in $(seq 1 27); do "$OUT/decode-check" window "$k" > "$OUT/decode-window-$k.txt"; done
    cat "$OUT/decode-window-1.txt" "$OUT/decode-window-27.txt"
    # The end-to-end part needs a recording of a phone's 800x480 cluster stream (Annex-B H.264);
    # such a recording shows the phone owner's location, so none is part of the repository.
    # Put your own at replays/aa-cluster-800x480.h264 (GEM "Save diagnostics" with a card that
    # has mst2-cluster-nav-capture-on in its root records one as mst2-cluster-nav-capture.h264).
    REPLAY="${MCN_AA_REPLAY:-$HERE/../replays/aa-cluster-800x480.h264}"
    if [ -s "$REPLAY" ]; then
        "$OUT/decode-check" "$REPLAY"
    else
        echo "decode: no cluster stream recording at $REPLAY, end-to-end part skipped"
    fi
else
    echo "aa hooks: no 32-bit host toolchain, skipped"
fi

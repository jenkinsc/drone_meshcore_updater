# Reference vectors from MeshCore itself

`ref.cpp` links MeshCore's own `src/Utils.cpp`, `src/Identity.cpp` and
`lib/ed25519` (plus rweather's Crypto library, which MeshCore depends on) and
prints the values in `../vectors/meshcore_vectors.txt`. The host tests in
`../test_mc.c` must reproduce them byte for byte, which is what ties this
updater's wire format to MeshCore's rather than to a reading of its docs.

Generated against MeshCore commit `a366955cb2f67b8e6842d4f00d2b6a554dddd88a`.
Identities come from fixed seeds (`FixedRNG`: byte i = base + 7*i + 3, with base
0x10 for the node and 0x55 for the operator); they are test keys, not secrets.

To regenerate (needs checkouts of meshcore-dev/MeshCore and rweather/arduinolibs,
and a tiny `Stream.h` stub because MeshCore's headers include the Arduino one):

    g++ -std=c++17 -w -Istub -I$CRYPTO -I$MC/src -I$MC/lib/ed25519 ref.cpp \
        $MC/src/Utils.cpp $MC/src/Identity.cpp $MC/lib/ed25519/*.c \
        $CRYPTO/{AES128,AESCommon,BlockCipher,SHA256,SHA512,Hash,Crypto,Ed25519,Curve25519,BigNumberUtil}.cpp \
        -Wl,--unresolved-symbols=ignore-all -no-pie -o ref && ./ref

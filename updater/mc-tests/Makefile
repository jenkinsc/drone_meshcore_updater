# Host tests for the MeshCore command layer. Needs a C compiler and OpenSSL
# headers (libssl-dev). Run from this directory: `make test`.
CC      ?= cc
CFLAGS  ?= -std=c99 -O1 -g -Wall -Wextra -fsanitize=address,undefined -fno-sanitize=shift -fno-omit-frame-pointer
SRC     = ../src/mc
ED      = $(SRC)/ed25519
CORE    = $(SRC)/mc_gate.c $(SRC)/mc_cmd.c $(SRC)/mc_progress.c $(SRC)/mc_ctl.c \
          $(SRC)/mc_evlog.c $(SRC)/mc_replay.c $(SRC)/mc_wire.c $(SRC)/mc_id.c \
          $(SRC)/mc_node.c $(SRC)/mc_crypto.c $(SRC)/mc_sx126x.c $(SRC)/mc_store.c
EDSRC   = $(ED)/fe.c $(ED)/ge.c $(ED)/sc_small.c $(ED)/sha512.c $(ED)/sign.c \
          $(ED)/keypair.c $(ED)/key_exchange.c $(ED)/ge_small.c

test_mc: test_mc.c crypto_openssl.c $(CORE) $(EDSRC)
	$(CC) $(CFLAGS) -DED25519_NO_SEED -DMC_ED25519_SMALL -o $@ test_mc.c crypto_openssl.c $(CORE) $(EDSRC) -lcrypto

test_ed_cross: test_ed_cross.c $(ED)/fe.c $(ED)/ge.c $(ED)/sc.c $(ED)/sc_small.c $(ED)/sha512.c $(ED)/ge_small.c
	$(CC) $(CFLAGS) -DED25519_NO_SEED -o $@ test_ed_cross.c $(ED)/fe.c $(ED)/ge.c $(ED)/sc.c $(ED)/sc_small.c $(ED)/sha512.c $(ED)/ge_small.c

test: test_mc test_ed_cross
	./test_ed_cross
	./test_mc

clean:
	rm -f test_mc test_ed_cross
.PHONY: test clean

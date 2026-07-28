# shairkindle — standalone AirPlay-1 (RAOP) receiver daemon for the Kindle Keyboard (K3).
# Self-contained: BearSSL + kd_v6emul are vendored in-tree under vendor/ (see
# vendor/*/VENDORED.md for provenance). Host tests build with cc; the device binary
# cross-builds with zig (static musl ARMv6).

CC     ?= cc
CXX    ?= c++
CFLAGS ?= -O2 -std=gnu11 -Wall -Wextra -Isrc

# --- vendored dependencies (see vendor/*/VENDORED.md) ---
BEARSSL   = vendor/bearssl

TESTS = tests/test_smoke tests/test_aes_cbc tests/test_volume tests/test_daemon tests/test_sink tests/test_oaep tests/test_apple_response tests/test_txt tests/test_alac tests/test_sdp tests/test_seqext tests/test_rtsp_msg tests/test_rtsp tests/test_raop_clock tests/test_jitter tests/test_rtp_wire tests/test_rtp_sock tests/test_session_decode tests/test_session_thread tests/test_session_flush tests/test_mdns_txt tests/test_daap tests/test_bodycap tests/test_supervisor tests/test_sink_aplay tests/test_npstate tests/test_nprender tests/test_dacp_request tests/test_dacp_state tests/test_dacp_srv tests/test_dacp_debounce tests/test_raop_name

tests/test_smoke: tests/test_smoke.c src/version.h
	$(CC) $(CFLAGS) -o $@ tests/test_smoke.c

tests/test_volume: tests/test_volume.c src/raop_volume.c
	$(CC) $(CFLAGS) -o $@ $^

# Pure TXT-record builder (no external deps). tinysvcmdns is not required by this unit test.
tests/test_txt: tests/test_txt.c src/raop_txt.c
	$(CC) $(CFLAGS) -o $@ $^

tests/test_raop_name: tests/test_raop_name.c src/raop_name.c
	$(CC) $(CFLAGS) -o $@ $^

# Pure TXT-wire (length-prefixing) conversion, in its own TU (raop_mdns_wire.c) so this
# test links without the live raop_mdns.c mdnsd_* references; live mDNS is covered
# by the complete daemon build.
tests/test_mdns_txt: tests/test_mdns_txt.c src/raop_mdns_wire.c
	$(CC) $(CFLAGS) -o $@ $^

tests/test_daemon: tests/test_daemon.c src/daemon.c
	$(CC) $(CFLAGS) -o $@ $^

tests/test_supervisor: tests/test_supervisor.c src/supervisor.c
	$(CC) $(CFLAGS) -o $@ $^

# --- DACP player control: Kindle buttons -> sender transport ---
# The three DACP modules are mutually dependent (dacp.c calls state+resolve;
# resolve calls dacp.c's mono_ms; state is -pthread), so every dacp test links
# the whole set. Only the pure functions are exercised by the host tests; the
# socket/thread symbols are validated on-device.
DACP_SRC = src/dacp.c src/dacp_state.c src/dacp_resolve.c

tests/test_dacp_request: tests/test_dacp_request.c $(DACP_SRC)
	$(CC) $(CFLAGS) -pthread -o $@ $^

tests/test_dacp_state: tests/test_dacp_state.c $(DACP_SRC)
	$(CC) $(CFLAGS) -pthread -o $@ $^

tests/test_dacp_srv: tests/test_dacp_srv.c $(DACP_SRC)
	$(CC) $(CFLAGS) -pthread -o $@ $^

tests/test_dacp_debounce: tests/test_dacp_debounce.c $(DACP_SRC)
	$(CC) $(CFLAGS) -pthread -o $@ $^

tests/supervisor-hostcheck: src/supervisor_main.c src/supervisor.c
	$(CC) $(CFLAGS) -o $@ $^

tests/test_daap: tests/test_daap.c src/daap.c
	$(CC) $(CFLAGS) -o $@ tests/test_daap.c src/daap.c

tests/test_bodycap: tests/test_bodycap.c src/bodycap.c
	$(CC) $(CFLAGS) -o $@ tests/test_bodycap.c src/bodycap.c

# npstate is the RTSP<->render-worker state hand-off: a mutex-guarded snapshot,
# so this test links -pthread. daap.h is header-only here (daap_meta_t is a
# plain struct); daap_parse isn't called, so daap.c doesn't need linking.
tests/test_npstate: tests/test_npstate.c src/npstate.c
	$(CC) $(CFLAGS) -pthread -o $@ tests/test_npstate.c src/npstate.c

# fake_nprender is a stand-in renderer: exec'd by nprender's worker in place
# of the real fbink-driving script, so the render-worker test never touches
# real hardware. It just logs its own argv -- see tests/fake_nprender.c.
tests/fake_nprender: tests/fake_nprender.c
	$(CC) $(CFLAGS) -o $@ $^

# nprender is the off-loop render worker (its own pthread; reads npstate via
# np_get, never shares npstate's lock). Links npstate.c (state source) and
# depends on fake_nprender being built (pointed at via AIRPLAY_NOWPLAYING_BIN
# at test run time -- not passed on this compile line).
tests/test_nprender: tests/test_nprender.c src/nprender.c src/npstate.c tests/fake_nprender
	$(CC) $(CFLAGS) -pthread -o $@ tests/test_nprender.c src/nprender.c src/npstate.c

# Links the record backend (SINK_RECORD_BACKEND), NOT KKBAudio: sink.cpp's #ifdef
# swaps in an in-process PCM recorder for host tests.
tests/test_sink: tests/test_sink.cpp src/sink.cpp
	$(CXX) -O2 -std=gnu++17 -Wall -DSINK_RECORD_BACKEND -Isrc -o $@ $^

tests/fake_consumer: tests/fake_consumer.c
	$(CC) -O2 -Wall -o $@ $^

# Links sink.cpp WITHOUT SINK_RECORD_BACKEND -- this is the real aplay-pipe backend
# same code the device build (raopd-armv6) compiles.
tests/test_sink_aplay: tests/test_sink_aplay.c src/sink.cpp tests/fake_consumer
	$(CXX) -O2 -std=gnu++17 -Wall -Isrc -x c++ -o $@ tests/test_sink_aplay.c src/sink.cpp

BEARSSL_INC = -I$(BEARSSL)/inc -I$(BEARSSL)/src
BEARSSL_SRC = $(shell find $(BEARSSL)/src -name '*.c')
BEARSSL_OBJ = $(BEARSSL_SRC:.c=.o)
BEARSSL_A   = $(BEARSSL)/libbearssl.a
CFLAGS += -I$(BEARSSL)/inc

$(BEARSSL)/src/%.o: $(BEARSSL)/src/%.c
	$(CC) -O2 $(BEARSSL_INC) -c $< -o $@
$(BEARSSL_A): $(BEARSSL_OBJ)
	ar rcs $@ $(BEARSSL_OBJ)

tests/test_aes_cbc: tests/test_aes_cbc.c tests/fixture_aes.c src/aes_cbc.c $(BEARSSL_A)
	$(CC) $(CFLAGS) -o $@ tests/test_aes_cbc.c tests/fixture_aes.c src/aes_cbc.c $(BEARSSL_A)

tests/test_oaep: tests/test_oaep.c tests/fixture_oaep.c src/rsa_oaep.c src/airport_key.c $(BEARSSL_A)
	$(CC) $(CFLAGS) -o $@ tests/test_oaep.c tests/fixture_oaep.c src/rsa_oaep.c src/airport_key.c $(BEARSSL_A)

tests/test_apple_response: tests/test_apple_response.c tests/fixture_apple.c src/apple_response.c src/airport_key.c $(BEARSSL_A)
	$(CC) $(CFLAGS) -o $@ tests/test_apple_response.c tests/fixture_apple.c src/apple_response.c src/airport_key.c $(BEARSSL_A)

# Vendored hairtunes alac.c emits warnings under -Wall -Wextra, so compile ONLY it
# with -w (warnings off) — our own code keeps the full warning set via $(CFLAGS).
vendor/alac/alac.o: vendor/alac/alac.c vendor/alac/alac.h
	$(CC) -O2 -std=gnu11 -w -Ivendor/alac -c $< -o $@

tests/test_alac: tests/test_alac.c tests/fixture_alac.c src/alac_shim.c vendor/alac/alac.o
	$(CC) $(CFLAGS) -Ivendor/alac -o $@ tests/test_alac.c tests/fixture_alac.c src/alac_shim.c vendor/alac/alac.o

# Session core: real BearSSL unwrap/decrypt + real ALAC decode + the fake sink
# (KKBAUDIO_HOST_TEST). alac_shim.c/session.c/aes_cbc.c/rsa_oaep.c/airport_key.c/the
# fixture rely on C semantics (e.g. alac_shim.c's implicit void*->T* calloc() assigns)
# that clang's C++ frontend rejects, so -- unlike test_sink/test_oaep -- this can't be
# one $(CXX) command over mixed .c/.cpp sources: each .c compiles to its own object
# with $(CC), the two real C++ sources (the fake sink, sink.cpp) compile with $(CXX),
# then $(CXX) links everything (needed for KKBAudioClass's C++ runtime deps).
SDEC_OBJS = tests/sdec-test.o tests/sdec-session.o tests/sdec-aes_cbc.o \
            tests/sdec-rsa_oaep.o tests/sdec-airport_key.o tests/sdec-alac_shim.o \
            tests/sdec-fixture_oaep.o tests/sdec-sink.o

tests/sdec-test.o: tests/test_session_decode.c tests/fixture_session_pcm.inc src/session.h
	$(CC) $(CFLAGS) -c tests/test_session_decode.c -o $@
tests/sdec-session.o: src/session.c src/session.h
	$(CC) $(CFLAGS) -pthread -c src/session.c -o $@
tests/sdec-aes_cbc.o: src/aes_cbc.c
	$(CC) $(CFLAGS) -c src/aes_cbc.c -o $@
tests/sdec-rsa_oaep.o: src/rsa_oaep.c
	$(CC) $(CFLAGS) -c src/rsa_oaep.c -o $@
tests/sdec-airport_key.o: src/airport_key.c
	$(CC) $(CFLAGS) -c src/airport_key.c -o $@
tests/sdec-alac_shim.o: src/alac_shim.c
	$(CC) $(CFLAGS) -Ivendor/alac -c src/alac_shim.c -o $@
tests/sdec-fixture_oaep.o: tests/fixture_oaep.c
	$(CC) $(CFLAGS) -c tests/fixture_oaep.c -o $@
tests/sdec-sink.o: src/sink.cpp
	$(CXX) -O2 -std=gnu++17 -Wall -DSINK_RECORD_BACKEND -Isrc -c src/sink.cpp -o $@

# session.c pulls in the rtp/seqext/jitter/raop_clock core, so the decode test links
# them too (built plain; the small-fill knob is thread-test only).
SDEC_CORE = tests/sthr-rtp.o tests/sthr-seqext.o tests/sthr-jitter.o tests/sthr-raop_clock.o

tests/test_session_decode: $(SDEC_OBJS) $(SDEC_CORE) vendor/alac/alac.o $(BEARSSL_A)
	$(CXX) -pthread -o $@ $(SDEC_OBJS) $(SDEC_CORE) vendor/alac/alac.o $(BEARSSL_A)

# Session threads: same real-crypto/real-ALAC/fake-sink link shape as
# test_session_decode, plus the rtp/seqext/jitter/raop_clock core and -pthread. session.c
# is built with -DRAOP_TEST_SMALL_FILL so START_FILL_FRAMES is tiny (a couple packets start
# playback), keeping the threaded loopback test bounded.
STHR_OBJS = tests/sthr-test.o tests/sthr-session.o tests/sdec-aes_cbc.o \
            tests/sdec-rsa_oaep.o tests/sdec-airport_key.o tests/sdec-alac_shim.o \
            tests/sdec-fixture_oaep.o tests/sdec-sink.o \
            tests/sthr-rtp.o tests/sthr-seqext.o tests/sthr-jitter.o tests/sthr-raop_clock.o

tests/sthr-test.o: tests/test_session_thread.c tests/fixture_session_pcm.inc src/session.h
	$(CC) $(CFLAGS) -pthread -DRAOP_TEST_SMALL_FILL -c tests/test_session_thread.c -o $@
tests/sthr-session.o: src/session.c src/session.h
	$(CC) $(CFLAGS) -pthread -DRAOP_TEST_SMALL_FILL -c src/session.c -o $@
tests/sthr-rtp.o: src/rtp.c
	$(CC) $(CFLAGS) -pthread -c src/rtp.c -o $@
tests/sthr-seqext.o: src/seqext.c
	$(CC) $(CFLAGS) -pthread -c src/seqext.c -o $@
tests/sthr-jitter.o: src/jitter.c
	$(CC) $(CFLAGS) -pthread -c src/jitter.c -o $@
tests/sthr-raop_clock.o: src/raop_clock.c
	$(CC) $(CFLAGS) -pthread -c src/raop_clock.c -o $@

tests/test_session_thread: $(STHR_OBJS) vendor/alac/alac.o $(BEARSSL_A)
	$(CXX) -pthread -o $@ $(STHR_OBJS) vendor/alac/alac.o $(BEARSSL_A)

# FLUSH barrier: same link shape as test_session_thread; reuses the SMALL_FILL
# session/core objects. Only the test .o is new.
SFLU_OBJS = tests/sflu-test.o tests/sthr-session.o tests/sdec-aes_cbc.o \
            tests/sdec-rsa_oaep.o tests/sdec-airport_key.o tests/sdec-alac_shim.o \
            tests/sdec-fixture_oaep.o tests/sdec-sink.o \
            tests/sthr-rtp.o tests/sthr-seqext.o tests/sthr-jitter.o tests/sthr-raop_clock.o

tests/sflu-test.o: tests/test_session_flush.c tests/fixture_session_pcm.inc src/session.h
	$(CC) $(CFLAGS) -pthread -DRAOP_TEST_SMALL_FILL -c tests/test_session_flush.c -o $@

tests/test_session_flush: $(SFLU_OBJS) vendor/alac/alac.o $(BEARSSL_A)
	$(CXX) -pthread -o $@ $(SFLU_OBJS) vendor/alac/alac.o $(BEARSSL_A)

tests/test_sdp: tests/test_sdp.c src/sdp.c src/b64.c
	$(CC) $(CFLAGS) -o $@ tests/test_sdp.c src/sdp.c src/b64.c

tests/test_seqext: tests/test_seqext.c src/seqext.c
	$(CC) $(CFLAGS) -o $@ $^

tests/test_rtsp_msg: tests/test_rtsp_msg.c src/rtsp_msg.c
	$(CC) $(CFLAGS) -o $@ $^

tests/test_rtsp: tests/test_rtsp.c src/rtsp.c src/rtsp_msg.c src/sdp.c src/b64.c src/apple_response.c src/airport_key.c src/raop_volume.c $(BEARSSL_A)
	$(CC) $(CFLAGS) -o $@ tests/test_rtsp.c src/rtsp.c src/rtsp_msg.c src/sdp.c src/b64.c src/apple_response.c src/airport_key.c src/raop_volume.c $(BEARSSL_A)

tests/test_raop_clock: tests/test_raop_clock.c src/raop_clock.c
	$(CC) $(CFLAGS) -o $@ $^

tests/test_rtp_wire: tests/test_rtp_wire.c src/rtp.c src/raop_clock.c
	$(CC) $(CFLAGS) -o $@ $^

tests/test_rtp_sock: tests/test_rtp_sock.c src/rtp.c src/raop_clock.c
	$(CC) $(CFLAGS) -o $@ $^

tests/test_jitter: tests/test_jitter.c src/jitter.c src/seqext.c
	$(CC) $(CFLAGS) -o $@ $^

# --- host smoke build: the real daemon linked for the loopback RTSP test ---
# main.c references session_*/sink_*/raop_mdns_* (compiled in, even though the
# --smoke path never reaches RECORD/mDNS), so the host binary must LINK the whole
# stack. Same split-compile pattern as test_session_decode: OUR C with the full
# warning set, the C++ sink (SINK_RECORD_BACKEND),
# the vendored tinysvcmdns with -w, then link with $(CXX).
SMOKE_CSRC = main rtsp rtsp_msg sdp b64 apple_response airport_key raop_volume \
             aes_cbc rsa_oaep rtp session seqext jitter raop_clock alac_shim \
             daemon raop_txt wakelock daap bodycap npstate nprender \
             dacp dacp_state dacp_resolve raop_name
SMOKE_COBJ = $(SMOKE_CSRC:%=tests/smoke-%.o) tests/smoke-raop_mdns.o

tests/smoke-%.o: src/%.c
	$(CC) $(CFLAGS) -pthread -Ivendor/alac -Ivendor/tinysvcmdns -c $< -o $@
# raop_mdns.c includes the vendored mdnsd.h, which forward-declares struct
# rr_entry in a prototype (-Wvisibility). That warning is the vendored header's,
# not our logic, so this one TU builds with -w (mirrors the device -w build).
tests/smoke-raop_mdns.o: src/raop_mdns.c
	$(CC) -O2 -std=gnu11 -w -Isrc -Ivendor/tinysvcmdns -c $< -o $@
tests/smoke-sink.o: src/sink.cpp
	$(CXX) -O2 -std=gnu++17 -Wall -DSINK_RECORD_BACKEND -Isrc -c $< -o $@
tests/smoke-mdns.o: vendor/tinysvcmdns/mdns.c
	$(CC) -O2 -std=gnu11 -w -Ivendor/tinysvcmdns -c $< -o $@
tests/smoke-mdnsd.o: vendor/tinysvcmdns/mdnsd.c
	$(CC) -O2 -std=gnu11 -w -Ivendor/tinysvcmdns -c $< -o $@

SMOKE_OBJS = $(SMOKE_COBJ) tests/smoke-sink.o \
             tests/smoke-mdns.o tests/smoke-mdnsd.o
tests/smoke-raopd: $(SMOKE_OBJS) vendor/alac/alac.o $(BEARSSL_A)
	$(CXX) -pthread -o $@ $(SMOKE_OBJS) vendor/alac/alac.o $(BEARSSL_A)

tests/rtsp_probe: tests/rtsp_probe.c
	$(CC) $(CFLAGS) -o $@ $^

# --- Java host tests (kindlet shell) -------------------------------------
# JAVA_SRC's tree includes com.besteffortlabs.shairkindle.ShairKindle, which
# references the Kindle API (com.amazon.kindle.kindlet.*) -- so the clean-room
# stubs (kindlet/stubs/, never bundled into the .azw2) must compile alongside it.
JAVA_SRC   = kindlet/src
JAVA_STUBS = kindlet/stubs
JAVA_TEST  = kindlet/test
JAVA_OUT   = kindlet/test-classes
JAVA_TESTS = com.besteffortlabs.kindletshell.HarnessSelfTest com.besteffortlabs.kindletshell.ValueTypesTest com.besteffortlabs.kindletshell.LeaseConnectionTest com.besteffortlabs.kindletshell.PayloadInstallerTest com.besteffortlabs.kindletshell.NativeLauncherTest com.besteffortlabs.shairkindle.NowPlayingEncoderTest com.besteffortlabs.kindletshell.KindletShellInterlockTest com.besteffortlabs.shairkindle.TeardownSequenceTest

.PHONY: check-java
check-java:
	rm -rf $(JAVA_OUT) && mkdir -p $(JAVA_OUT)
	javac -nowarn -source 1.4 -target 1.4 -d $(JAVA_OUT) \
	  $(shell find $(JAVA_SRC) $(JAVA_STUBS) $(JAVA_TEST) -name '*.java' 2>/dev/null)
	@for t in $(JAVA_TESTS); do echo "== $$t =="; java -cp $(JAVA_OUT) $$t || exit 1; done

.PHONY: check clean
check: $(TESTS) tests/smoke-raopd tests/rtsp_probe tests/supervisor-hostcheck check-java
	@set -e; for t in $(TESTS); do echo "== $$t =="; ./$$t; done
	@echo "ALL C TESTS PASSED"
	@echo "== tests/test_toggle.sh =="
	@sh tests/test_toggle.sh
	@echo "== tests/test_wifi_toggle.sh =="
	@sh tests/test_wifi_toggle.sh
	@echo "== tests/test_debug_log.sh =="
	@sh tests/test_debug_log.sh
	@echo "== tests/test_name.sh =="
	@sh tests/test_name.sh
	@echo "== tests/test_rtsp_loopback.sh =="
	@sh tests/test_rtsp_loopback.sh
	@echo "== tests/test_capture.sh =="
	@sh tests/test_capture.sh
	@echo "== tests/test_airplay_off.sh =="
	@sh tests/test_airplay_off.sh
	@echo "== tests/test_airplay_teardown.sh =="
	@sh tests/test_airplay_teardown.sh
	@echo "== tests/test_startup_reconcile.sh =="
	@sh tests/test_startup_reconcile.sh
	@echo "== tests/test_supervisor_lifecycle.sh =="
	@sh tests/test_supervisor_lifecycle.sh
	@echo "== tests/test_payload_bundle.sh =="
	@sh tests/test_payload_bundle.sh

# --- device cross build: static soft-float ARMv6 for the K3, via zig ---
# ARM1136 is ARMv6, NOT ARMv6T2 — zig's arm1136jf_s model wrongly enables v6t2, so LLVM
# can emit MOVW/MOVT (undefined on ARM1136 -> SIGILL). Use -mcpu=arm1136jf_s-v6t2 to
# disable it (literal pools instead), and link the kd_v6emul SIGILL-emulation shim
# (inert on K3, needed on the shared K2 armv6 payload). Mirrors kinduino/fbinkd/Makefile.
ZIG      ?= zig
TARGET    = arm-linux-musleabi
MCPUFLAG  = arm1136jf_s-v6t2
V6EMUL    = -DKD_V6EMUL_FORCE vendor/kd_v6emul/kd_v6emul.c
OUTDIR   ?= out-armv6

# The aplay-over-dmix sink (src/sink.cpp) shells out to /usr/bin/aplay via posix_spawn,
# so KKBAudio/tinyalsa are no longer linked (nothing in src/ references them).
DEV_INC   = -Isrc -I$(BEARSSL)/inc -Ivendor/alac -Ivendor/tinysvcmdns
DEV_DEF   = -DARDUINO_ARMV6_K3
DEV_CSRC  = src/main.c src/aes_cbc.c src/rsa_oaep.c src/apple_response.c src/airport_key.c \
            src/raop_volume.c src/daemon.c src/raop_txt.c src/alac_shim.c \
            src/rtp.c src/session.c src/seqext.c src/jitter.c src/raop_clock.c \
            src/rtsp.c src/rtsp_msg.c src/sdp.c src/b64.c \
            src/raop_mdns.c src/raop_mdns_wire.c src/wakelock.c \
            src/daap.c src/bodycap.c src/npstate.c src/nprender.c \
            src/dacp.c src/dacp_state.c src/dacp_resolve.c src/raop_name.c
DEV_CXX   = src/sink.cpp
DEV_VEND  = $(wildcard vendor/alac/*.c) $(wildcard vendor/tinysvcmdns/*.c)

# -w: zig's own libc++ headers emit system-header nullability warnings on this target;
# our code stays warning-clean under `make check` (host tests keep -Wall -Wextra).
.PHONY: raopd-armv6
raopd-armv6:
	@mkdir -p $(OUTDIR)
	$(ZIG) c++ -target $(TARGET) -mcpu=$(MCPUFLAG) -static -Os -w $(DEV_DEF) $(V6EMUL) \
	  $(DEV_INC) \
	  $(DEV_CSRC) $(DEV_CXX) $(DEV_VEND) \
	  $(BEARSSL)/precompiled/arm1136jf_s/libBearSSL.a \
	  -pthread -o $(OUTDIR)/raopd
	file $(OUTDIR)/raopd

.PHONY: supervisor-armv6
supervisor-armv6:
	@mkdir -p $(OUTDIR)
	$(ZIG) c++ -target $(TARGET) -mcpu=$(MCPUFLAG) -static -Os -w $(DEV_DEF) $(V6EMUL) \
	  -Isrc src/supervisor_main.c src/supervisor.c \
	  -pthread -o $(OUTDIR)/airplay-supervisor
	file $(OUTDIR)/airplay-supervisor

clean:
	rm -f $(TESTS) $(OUTDIR)/raopd vendor/alac/alac.o $(SDEC_OBJS) $(STHR_OBJS) $(SFLU_OBJS)
	rm -f $(SMOKE_OBJS) tests/smoke-raopd tests/rtsp_probe tests/supervisor-hostcheck
	rm -f tests/fake_consumer tests/fake_nprender
	rm -f $(BEARSSL_OBJ) $(BEARSSL_A)
	rm -rf $(OUTDIR)

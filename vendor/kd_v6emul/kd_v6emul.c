// kd_v6emul.c — SIGILL-emulate the ARMv6K SUB-WORD exclusives + barriers that the
// i.MX31 (Kindle 2, and the Kindle DX) lacks. The i.MX31 is ARM1136JF-S r0 = base
// ARMv6: WORD ldrex/strex are NATIVE (base ARMv6, they run on the silicon), but the
// BYTE/HALF exclusives (LDREXB/STREXB/LDREXH/STREXH), CLREX, and the DMB/DSB/ISB
// barriers are ARMv6K additions -> UNDEFINED on r0 -> SIGILL. zig/LLVM emit those for
// -mcpu=arm1136jf_s (e.g. zig std SmpAllocator / musl byte locks) and they're baked
// into zig's bundled musl too, which we can't recompile. (Verified on-device: an ISA
// probe shows ldrex/strex native; ldrexb/strexb/ldrexh/strexh SIGILL. So musl a_cas /
// pthread_mutex / atomic_int — all WORD-sized — were never broken on the K2; the shim
// exists for the missing SUB-WORD exclusives + barriers.)
//
// The K2/DX are SINGLE-CORE, so we trap the undefined instruction and emulate it:
//   - LDREX/LDREXH/LDREXB  -> load the CONTAINING ALIGNED WORD, hand Rt the sized
//                             sub-word (little-endian), and shadow the whole word in a
//                             per-thread (TLS) exclusive-monitor.
//   - STREX/STREXH/STREXB  -> a real compare-and-swap of that whole word (splicing the
//                             new sub-word in) via the kernel __kuser_cmpxchg helper —
//                             genuine mutual exclusion under multi-thread contention,
//                             for BYTE/HALF as well as word. Gated on a matching
//                             monitor (addr + size); a mismatch fails the STREX (Rd=1)
//                             so the CAS loop retries. If the helper is unavailable at
//                             init (never on the 2.6.22 target) it falls back to a word
//                             store (degraded but never bricks). NB word ldrex/strex are
//                             native here, so their branch is effectively dead code on
//                             this silicon — handled uniformly, defensively.
//   - DMB/DSB/ISB/CLREX     -> no-op / clear-monitor (barriers are meaningless on UP).
// then advance PC past the 4-byte ARM instruction and resume.
//
// Installed as an early constructor so it's live before the first atomic (musl
// init + program init have been observed to fault only AFTER this runs).
//
// SCOPE: this must land in every armv6 binary that runs on the i.MX31 (K2/DX)
// and NOWHERE else. Two activation paths, one source:
//   - Sketch ELFs: this file is a `kindle` core source, guarded on
//     ARDUINO_ARMV6_K2 (the platform's -DARDUINO_{build.board} macro), so it is
//     an EMPTY object for K3 (i.MX35 = real ARMv6K) and every armv7 board.
//     main.cpp holds a link anchor (see kd_v6emul_anchor) so lld pulls this
//     constructor-only object out of the core archive.
//   - Payload tools (kinduino-fbinkd/-launch/-watch/…): built per-ARCH, not per
//     model, so there is no board macro. Those armv6 builds compile this file
//     with -DKD_V6EMUL_FORCE (see device/*/build_*.sh, fbinkd/Makefile). The
//     armv6 payload is SHARED with the K3 — the shim is inert there (those
//     instructions are native on i.MX35, so the handler never fires).
//
// HW-validated on a real Kindle 2 (i.MX31, ARM1136 r0p4), 2026-07-10: fbink
// relinked with this renders to the einkfb; overhead is ~6-12 emulated
// instructions per fbink run (atomics live in malloc/lock, not pixel loops).
//
// CAVEATS: the emulation is CAS-correct but not full exclusive-monitor semantics
// (ABA-permissive — a same-value A->B->A cycle on the peer thread between our LDREX
// and STREX succeeds where a real monitor would usually fail; harmless for
// mutex/refcount/CAS-loop words, which is the whole target). Byte/half CAS operates at
// WORD granularity: a STREXB also fails if an ADJACENT byte in the same word changed —
// stricter than a per-byte monitor, but safe (the CAS loop just retries) and it matches
// how real cacheline-granular monitors behave. Sub-word exclusives are assumed naturally
// aligned — as every C11 atomic and compiler-emitted LDREX* is; a misaligned LDREXH/STREXH
// is architecturally UNDEFINED and unsupported (the word-mask captures the wrong bytes).
// ARM-state encodings only (add Thumb-2 decode if any Thumb LDREXB ever surfaces).
// LDREXD/STREXD (64-bit) not handled (unused by 32-bit atomics); add if needed. The
// handler is allocation-free and lock-free.
#if defined(ARDUINO_ARMV6_K2) || defined(KD_V6EMUL_FORCE)
#define _GNU_SOURCE
#include <signal.h>
#include <ucontext.h>
#include <stdint.h>
#include <unistd.h>

// __kuser_cmpxchg: kernel-provided CAS at a fixed address, atomic on UP ARMv6-non-K.
// Returns 0 on success (swapped), nonzero on failure. Version word at 0xffff0ffc.
typedef int (*kuser_cmpxchg_t)(int oldval, int newval, volatile int* ptr);
#define KUSER_CMPXCHG   ((kuser_cmpxchg_t) 0xffff0fc0u)
#define KUSER_VERSION   (*(volatile uint32_t*) 0xffff0ffcu)
static int kd_cas_ok = 0;   // set at init iff the helper is usable (version >= 2)

// Per-thread exclusive-monitor shadow. TLS so two threads interleaving LDREX/STREX
// on one core don't clobber a single global. initial-exec model => no lazy alloc,
// safe to touch from the signal handler.
struct kd_monitor { void* addr; uint32_t expected; unsigned char size; unsigned char valid; };
static __thread struct kd_monitor kd_mon __attribute__((tls_model("initial-exec")));

// Link anchor: referenced by main.cpp (under ARDUINO_ARMV6_K2) so lld pulls this
// object out of the static core archive. A member whose only content is a
// constructor is otherwise dropped — nothing references its symbols. The payload
// builds compile this file directly (not from an archive), so the anchor is inert
// there but harmless.
int kd_v6emul_anchor = 0;

static void kd_v6emul_handler(int sig, siginfo_t* si, void* p) {
	(void) sig;
	(void) si;
	ucontext_t*    uc = p;
	unsigned long* R  = &uc->uc_mcontext.arm_r0;    // arm_r0..arm_r15 are contiguous
	uint32_t       in = *(volatile uint32_t*) uc->uc_mcontext.arm_pc;
	uint32_t       a = in & 0x0ff00fffu, b = in & 0x0ff00ff0u, c = in & 0xfffffff0u;
	int            Rn = (in >> 16) & 0xf, Rt = (in >> 12) & 0xf, Rd = (in >> 12) & 0xf, Rm = in & 0xf;

	// Decode LDREX/LDREXH/LDREXB (a) and STREX/STREXH/STREXB (b) to a width in bytes.
	int ld_size = (a == 0x01900f9fu) ? 4 : (a == 0x01f00f9fu) ? 2 : (a == 0x01d00f9fu) ? 1 : 0;
	int st_size = (b == 0x01800f90u) ? 4 : (b == 0x01e00f90u) ? 2 : (b == 0x01c00f90u) ? 1 : 0;

	if (ld_size) {                                             // LDREX / LDREXH / LDREXB
		// Read the CONTAINING ALIGNED WORD (mask the address DOWN — always page-safe:
		// the aligned word of any valid address is mapped; exclusives are only ever on
		// normal RAM, never MMIO). Extract the sized value for Rt (little-endian: the
		// sub-word sits at (addr&3)*8) and shadow the WHOLE word as `expected`. A later
		// sub-word STREX CASes that whole word, splicing its new sub-word in — correct
		// exclusive-monitor semantics through the word-only __kuser_cmpxchg.
		unsigned long addr  = R[Rn];
		uint32_t      word  = *(volatile uint32_t*) (addr & ~3ul);
		unsigned      shift = (unsigned) (addr & 3ul) * 8u;
		uint32_t      mask  = (ld_size == 4) ? 0xFFFFFFFFu
		                    : (ld_size == 2) ? (0xFFFFu << shift) : (0xFFu << shift);
		R[Rt] = (word & mask) >> shift;
		kd_mon.addr = (void*) addr; kd_mon.expected = word;
		kd_mon.size = (unsigned char) ld_size; kd_mon.valid = 1;
	} else if (st_size) {                                      // STREX / STREXH / STREXB -> word CAS
		unsigned long addr = R[Rn];
		if (kd_mon.valid && kd_mon.addr == (void*) addr && kd_mon.size == (unsigned char) st_size) {
			unsigned shift = (unsigned) (addr & 3ul) * 8u;
			uint32_t mask  = (st_size == 4) ? 0xFFFFFFFFu
			               : (st_size == 2) ? (0xFFFFu << shift) : (0xFFu << shift);
			uint32_t neu   = (kd_mon.expected & ~mask) | (((uint32_t) R[Rm] << shift) & mask);
			if (kd_cas_ok) {
				int rc = KUSER_CMPXCHG((int) kd_mon.expected, (int) neu, (volatile int*) (addr & ~3ul));
				R[Rd] = rc ? 1 : 0;                             // 0=stored, 1=word changed -> retry
			} else {
				// Legacy (helper absent — never on the 2.6.22 target): store-succeed, but
				// store only the REQUESTED width so we can't clobber adjacent bytes.
				if (st_size == 1)      *(volatile uint8_t*)  addr = (uint8_t)  R[Rm];
				else if (st_size == 2) *(volatile uint16_t*) addr = (uint16_t) R[Rm];
				else                   *(volatile uint32_t*) addr = (uint32_t) R[Rm];
				R[Rd] = 0;
			}
		} else {
			R[Rd] = 1;                                          // no matching monitor -> FAIL, no store
		}
		kd_mon.valid = 0;
	} else if (c == 0xf57ff050u || c == 0xf57ff040u || c == 0xf57ff060u) { /* DMB/DSB/ISB = nop */ }
	else if (in == 0xf57ff01fu) { kd_mon.valid = 0; /* CLREX clears the monitor */ }
	else {
		static const char m[] = "kd_v6emul: unhandled undefined instruction\n";
		write(2, m, sizeof m - 1);
		_exit(132);
	}
	uc->uc_mcontext.arm_pc += 4;
}

__attribute__((constructor(101))) static void kd_v6emul_init(void) {
	kd_cas_ok = (KUSER_VERSION >= 2u);   // 2.6.22 guarantees >=2; fail closed otherwise
	if (!kd_cas_ok) {
		static const char w[] = "kd_v6emul: __kuser_cmpxchg unavailable; STREX degraded to store-succeed\n";
		write(2, w, sizeof w - 1);
	}
	struct sigaction sa;
	sa.sa_sigaction = kd_v6emul_handler;
	sa.sa_flags     = SA_SIGINFO;        // NB: no SA_NODEFER -> a recursive SIGILL fails closed
	sigemptyset(&sa.sa_mask);
	sigaction(SIGILL, &sa, (void*) 0);
}
#endif  // ARDUINO_ARMV6_K2 || KD_V6EMUL_FORCE

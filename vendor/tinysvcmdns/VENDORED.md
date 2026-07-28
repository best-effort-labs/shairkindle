# Vendored: tinysvcmdns

Source: https://github.com/geekman/tinysvcmdns (Darell Tan's original — the minimal
single-file mDNS responder for publishing services; mirrored from the upstream
`bitbucket.org/geekman/tinysvcmdns`).

Commit: `84b40311d00bf86ad2b79ee9df7121aacf5a6e61` (2018-01-16).

License: BSD-3-Clause (see `LICENSE.txt`).

Files: `mdns.c`, `mdns.h` (the responder + packet build/parse core), `mdnsd.c`, `mdnsd.h`
(the small socket-owning daemon wrapper).

## Local modifications

`mdnsd.c` is patched (2026-07-21) to fix a responder wedge seen on the K3: after a
streaming session, the mDNS thread got stuck in a blocking pipe read (`pipe_wait`),
stopped draining its socket, and the device vanished from all AirPlay pickers. Root
cause: the host process fork()+execs at RECORD (the wakelock's `system()`), and the
mDNS fds had no `FD_CLOEXEC`, so they leaked into the child; combined with a blocking
`read_pipe` after `select()` and an `else if` that skipped the socket, a fork/fd race
wedged the loop. Three targeted changes (all tagged `shairkindle:` in-source):
- `create_recv_sock`: `FD_CLOEXEC` on the mDNS socket.
- `create_pipe`: `FD_CLOEXEC` on both notify-pipe ends + `O_NONBLOCK` on the read end.
- `main_loop`: service the socket every iteration (`if`, not `else if`).

## Compilation status

The host `make check` suite compiles and tests only the pure, socket-free TXT builder
`src/raop_txt.c` (`raop_build_txt`); the vendored `mdns.c`/`mdnsd.c` are not in that
unit-test set. They ARE compiled and linked into the device `raopd` (`make raopd-armv6`),
where they drive the **live `_raop._tcp` advertisement**: `raop_mdns_start`/`raop_mdns_stop`
(in `src/raop_mdns.c`) bind a real mDNS socket via `mdnsd_*`, validated on the K3. So the
vendored responder is exercised on the device — just not by the host unit tests.

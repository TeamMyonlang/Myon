/*
 * Copyright 2026 TeamMyonlang
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

/*
 * Central platform-detection header for Myon.
 *
 * Historically the C sources scattered `#if defined(__linux__)` around every
 * place that touched a POSIX facility (sockets, select(), ucontext, stat(),
 * /dev/urandom, dlopen).  That accidentally treated *only* Linux as POSIX, so
 * macOS (which is a fully POSIX/BSD platform) fell through to the
 * "unsupported-platform" stubs even though every one of those facilities is
 * available on macOS.  The result was that networking, the async event loop,
 * FFI and the CSPRNG were all silently disabled on macOS.
 *
 * This header consolidates OS detection into a small, well-defined set of
 * feature macros so the rest of the codebase can ask *what a platform can do*
 * rather than hard-coding a single OS name.  The three platform families are:
 *
 *   MYON_OS_LINUX   - Linux (glibc / musl)
 *   MYON_OS_MACOS   - macOS / Darwin (Apple Silicon and Intel)
 *   MYON_OS_BSD     - the *BSDs (FreeBSD/OpenBSD/NetBSD/DragonFly)
 *   MYON_OS_WINDOWS - Windows (native MSVC, MSYS2/MinGW-w64, or a
 *                     MinGW-w64 cross build)
 *
 * and the umbrella:
 *
 *   MYON_OS_POSIX   - any Unix-like platform above (Linux, macOS, BSD).  This
 *                     is the correct guard for portable POSIX code that used to
 *                     be (wrongly) written as `#if defined(__linux__)`.
 *
 * Nothing here pulls in a system header, so it is safe to include very early
 * (before feature-test macros are even set) from any translation unit.
 */

#ifndef MYON_PLATFORM_H
#define MYON_PLATFORM_H

/* -------------------------------------------------------------------- */
/* OS family detection.                                                  */
/* -------------------------------------------------------------------- */

#if defined(_WIN32) || defined(_WIN64)
#  define MYON_OS_WINDOWS 1
#elif defined(__APPLE__) && defined(__MACH__)
#  define MYON_OS_MACOS 1
#elif defined(__linux__)
#  define MYON_OS_LINUX 1
#elif defined(__FreeBSD__) || defined(__OpenBSD__) || \
      defined(__NetBSD__)  || defined(__DragonFly__)
#  define MYON_OS_BSD 1
#endif

/* Umbrella: every Unix-like target shares the POSIX socket/select/stat/dlopen
 * surface, so portable POSIX code should key off MYON_OS_POSIX -- never off a
 * single OS name. */
#if defined(MYON_OS_LINUX) || defined(MYON_OS_MACOS) || defined(MYON_OS_BSD)
#  define MYON_OS_POSIX 1
#endif

/* -------------------------------------------------------------------- */
/* Capability flags derived from the OS family.                          */
/* -------------------------------------------------------------------- */

/*
 * ucontext-based cooperative coroutines (event loop).
 *
 * getcontext/makecontext/swapcontext exist on Linux glibc and on the
 * BSD/macOS C libraries.  They are marked "obsolescent" by POSIX and Apple
 * emits a deprecation warning unless you include <sys/ucontext.h> (rather than
 * <ucontext.h>) under _XOPEN_SOURCE -- but they remain fully functional, and
 * are the simplest portable fibre primitive short of hand-written assembly.
 * Windows has no ucontext; it uses the Win32 Fiber API instead.
 */
#if defined(MYON_OS_POSIX)
#  define MYON_HAVE_UCONTEXT 1
#endif

/*
 * dlopen/dlsym/dlclose run-time dynamic linking (C FFI).
 *
 * Present on every POSIX target: Linux needs -ldl, macOS/BSD ship it inside
 * libSystem/libc (no extra link flag).  Windows uses LoadLibrary instead.
 */
#if defined(MYON_OS_POSIX)
#  define MYON_HAVE_DLOPEN 1
#endif

/*
 * BSD sockets (myon.net / myon.http transport).  Available on every POSIX
 * target; Windows uses Winsock2.
 */
#if defined(MYON_OS_POSIX)
#  define MYON_HAVE_POSIX_SOCKETS 1
#endif

/*
 * /dev/urandom-style OS CSPRNG for myon.random.secure_*.  All POSIX targets
 * expose /dev/urandom; macOS/BSD additionally provide arc4random_buf(), which
 * we prefer there because it never fails and needs no file descriptor.
 */
#if defined(MYON_OS_POSIX)
#  define MYON_HAVE_DEV_URANDOM 1
#endif
#if defined(MYON_OS_MACOS) || defined(MYON_OS_BSD)
#  define MYON_HAVE_ARC4RANDOM 1
#endif

/*
 * send(MSG_NOSIGNAL) suppresses SIGPIPE on Linux, but the flag does not exist
 * on macOS/BSD.  There the equivalent is the per-socket SO_NOSIGPIPE option
 * (set once at socket-creation time).  net.c keys off these two flags so it
 * never references a symbol the platform does not define.
 */
#if defined(MYON_OS_LINUX)
#  define MYON_HAVE_MSG_NOSIGNAL 1
#endif
#if defined(MYON_OS_MACOS) || defined(MYON_OS_BSD)
#  define MYON_HAVE_SO_NOSIGPIPE 1
#endif

#endif /* MYON_PLATFORM_H */

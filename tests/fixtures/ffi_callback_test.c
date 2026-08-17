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
 * Tiny helper shared library for the Phase4.1 FFI callback regression test.
 * run_tests.sh builds this into tests/fixtures/libffi_callback_test.so before
 * running the suite.  Each function receives a Myon-registered callback (as a
 * long long (*)(...) pointer) and calls it back so the test can observe the
 * Myon side firing.
 */

typedef long long (*cb1)(long long);
typedef long long (*cb2)(long long, long long);
typedef long long (*cb0)(void);

/* Call the 1-arg callback twice: once with x, once with x+1. */
void call_twice(cb1 cb, long long x) {
    cb(x);
    cb(x + 1);
}

/* Call the 2-arg callback once and return whatever it returns. */
long long call_sum(cb2 cb, long long a, long long b) {
    return cb(a, b);
}

/* Call the 0-arg callback and return its result. */
long long call_noarg(cb0 cb) {
    return cb();
}

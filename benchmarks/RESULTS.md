# Benchmark results

`benchmarks/run.sh [runs] [solis-binary]`. Best of 7, i7-12700KF, gcc 16.
Solis is built `-O2 -DNDEBUG -flto`, the C side with `gcc -O2` (`c/`), Lua is
5.4 (`lua/`). "image" runs the precompiled `.slb`. Startup, compile and load
cost about 1 ms.

| bench   | original ms | now ms | C ms | Lua 5.4 ms | now vs C | speedup |
|---------|------------:|-------:|-----:|-----------:|---------:|--------:|
| loop    |        2025 |    447 | 56.7 |        618 |       8x |    4.5x |
| sieve   |        1758 |    584 | 41.1 |        620 |      14x |    3.0x |
| nbody   |         809 |    247 |  6.1 |        214 |      41x |    3.3x |
| arrays  |         352 |    103 |  7.8 |        112 |      13x |    3.4x |
| structs |         279 |    120 |  5.1 |        385 |      24x |    2.3x |
| fib     |         124 |     57 |  5.7 |         89 |      10x |    2.2x |
| maps    |         104 |    3.7 |  5.3 |        7.4 |       2x |     28x |
| strings |          42 |     40 | 10.2 |         40 |       4x |    1.0x |

Faster than Lua 5.4 on loop, sieve, arrays, fib, structs and maps; level on
strings; within 15% on nbody. Still 8-40x off C.

## What changed

1. **Store-forwarding stalls.** Values were copied as one 16-byte vector load but
   written as a 4-byte tag plus an 8-byte payload. A load spanning two recent
   stores cannot be forwarded, so a read right after a write to the same slot
   stalled about 12 cycles. Values now move as two scalar words
   (`vload`/`vstore`) and `sl_bool` writes the whole payload word.
2. **Conditional jumps compiled to `cmov`**, which makes the next instruction
   wait on the compare instead of on a branch prediction. `no-if-conversion` on
   `sl_run` took an empty loop from 0.68 s to 0.25 s.
3. **Threaded dispatch**: one indirect jump per handler. gcc folds them into one
   shared jump when the block looks cheap; a vectorised UTF-8 loop inside
   `sl_run` was defeating that, so it is out of line. clang threads on its own.
4. **Register bytecode.** The image still holds stack code (`compiler/emit.h`,
   version 8); `sl_load` translates each function once into register code, and
   `sl_run` executes that. The translator keeps locals, immediates and
   comparisons lazy until something needs them in a register, so `x + 1` is
   one `ADDI`, `i < n` followed by a branch is one compare-and-jump, and a
   scalar result is written straight into its local. Calls pass arguments in
   place: the callee's frame starts at the caller's argument registers. There
   is no per-push stack check, only one per call. Constants inside loops are
   loaded once on entry.
5. **Conditions compile as branches** (`gen_branch`): `a || b`, `a && b` and `!x`
   in `if`/`while` become jumps, not materialised booleans.
6. **Hash maps**, insertion-ordered, indexed past 8 pairs.
7. **Struct pool** for structs up to 8 fields, scalar fast paths in copy and
   release.

Checked against the stack VM it replaced with `tools/fuzz.sh`: 3,900 random
programs agree on output and leak counts, plus the conformance suite.

## Where the time goes now

- `i % n` is a 64-bit `idiv`. Moving to a 32-bit divide when both fit made no
  difference on this CPU.
- Every value is 16 bytes. Registers for scalar-only functions could be 8.
- `var b = bodies[i]` copies a struct (an allocation) and `bodies[i] = b`
  copies it back. Skipping the copy needs copy-on-write or liveness; the
  register VM makes copy-on-write feasible (`FSET` on a local sees `rc`
  directly) but it is not done.
- Calls: a frame push, a release scan over the callee's registers on return.
- Beyond this, the next step is emitting C.

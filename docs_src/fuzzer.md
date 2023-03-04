# XLS Fuzzer

[TOC]

To execute the XLS fuzz driver simply run a command line like the following:

```
bazel run -c opt \
  //xls/fuzzer:run_fuzz_multiprocess \
  -- --crash_path=/tmp/crashers-$(date +'%Y-%m-%d') --seed=0 --duration=8h
```

The XLS fuzzer generates a sequence of randomly generated DSLX functions and a
set of random inputs to each function often with interesting bit patterns.

Given that stimulus, the fuzz driver performs the following actions some of
which may be disabled/enabled via flags (run with `--help` for more details):

*   Runs the DSLX program through the DSLX interpreter with the batch of
    arguments
*   Converts the DSLX program to IR
*   Optimizes the converted IR
*   Interprets the pre-optimized and optimized IR with the batch of arguments
*   Generates the Verilog from the IR with randomly selected codegen options
*   Simulates the generated Verilog using the batch of arguments
*   Performs a multi-way comparison of the DSLX interpreter results, the
    pre-optimized IR interpreter results, post-optimized IR interpreter results,
    and the simulator results
*   If an issue is observed, the fuzz driver attempts to minimize the IR that
    causes an issue to occur.

The above actions are coordinated and run by the
[SampleRunner](https://github.com/google/xls/tree/main/xls/fuzzer/sample_runner.py) class.
Many actions are performed by invoking a separate binary which isolates any
crashes.

When miscompares in results occur or the generated function crashes part of XLS,
all artifacts generated by the fuzzer for that sample are written into a
uniquely-named subdirectory under the `--crash_path` given in the command line.
The fuzzer also writes a _crasher_ file which is a single file for reproducing
the issue. See [below](#debugging) for instructions on debugging a failing
sample.

## Crashers Directory

The crashers directory includes a subdirectory created for each failing sample.
To avoid collisions the subdirectory is named using a hash of the DSLX code.
Each crasher subdirectory has the following contents:

```
$ ls /tmp/crashers-2019-06-25/05adbd50
args.txt                   ir_converter_main.stderr   run.sh
cl.txt                     ir_minimizer.options.json  sample.ir
crasher_2020-04-23_9b05.x  ir_minimizer_test.sh       sample.ir.results
eval_ir_main.stderr        options.json               sample.x
exception.txt              opt_main.stderr            sample.x.results
```

The directory includes the problematic DSLX sample (`sample.x`) and the input
arguments (`args.txt`) as well as all artifacts generated and stderr output
emitted by the various utilities invoked to test the sample. Notable files
include:

*   `options.json` : Options used to run the sample.
*   `sample.ir` : Unoptimized IR generated from the DSLX sample.
*   `sample.opt.ir` : IR after optimizations.
*   `sample.v` : Generated Verilog.
*   `*.results` : The results (numeric values) produced by interpreting or
    simulating the respective input (DSLX, IR, or Verilog).
*   `exception.txt` : The exception raised when running the sample. Typically
    this will indicate either a result miscomparison or a tool return non-zero
    status (for example, the IR optimizer crashed).
*   `crasher_*.x`: A single file reproducer which includes the DSLX code,
    arguments, and options. See [below](#reproducers) for details.

Typically the exact nature of the failure can be identified by reading the file
`exception.txt` and possibly the stderr outputs of the various tools.

The fuzzer can optionally produce a minimized IR reproduction of the problem.
This will be written to `minimized.ir`. See [below](#minimization) for details.

### Single-file reproducers {#reproducers}

When the fuzzer encounters an issue it will create a single-file reproducer:

```
--- Worker 14 observed an exception, noting
--- Worker 14 noted crasher #1 for sampleno 42 at /tmp/crashers/095fb405
```

Copying that file to the directory `//xls/fuzzer/crashers` will
automatically create a bazel test target for it and add it to the regression
suite. Tests can also be added as known failures in
`//xls/fuzzer/build_defs.bzl` as they're being triaged /
investigated like so:

```
generate_crasher_regression_tests(
    srcs = glob(["crashers/*"]),
    prefix = "xls/fuzzer",
    # TODO(xls-team): 2019-06-30 Triage and fix these.
    failing = [
        "crashers/crasher_2019-06-29_129987.x",
        "crashers/crasher_2019-06-29_402110.x",
    ],
)
```

Known-failures are marked as manual and excluded from continuous testing.

To run the regression suite:

```
bazel test //xls/fuzzer:all
```

To run the regression suite including known-failures, run the regression target
directly:

```
bazel test //xls/fuzzer:regression_tests
```

To reproduce from that single-file reproducer there is a command line tool:

```
bazel run //xls/fuzzer:run_crasher -- \
  crasher_2019-06-26_3354.x
```

## IR minimization {#minimization}

By default the fuzzer attempts to generate a minimal IR reproducer for the
problem identified by the DSLX sample. Starting with the unoptimized IR the
fuzzer invokes `ir_minimizer_main` to reduce the size of the input IR. It uses
various simplification strategies to minimize the number of nodes in the IR. See
the usage description in the tool source code for detailed information.

The minimized IR is written to a file `minimized.ir` in the crasher directory
for the sample. Note that minimization is only possible if the problem (crash,
result miscomparison, etc.) occurs *after* conversion from DSLX to XLS IR.

## Summaries

To monitor progress of the fuzzer and to determine op coverage the fuzzer can
optionally (with `--summary_path`) write summary information to files. The
summary files are Protobuf files containing the proto `SampleSummaryProto`
defined in `//xls/fuzzer/sample_summary.proto`. The summary
information about the IR generated from the DSLX sample such as the number and
type of each IR op as well as the bit width and number of operands.

The summary information also includes a timing breakdown of the various
operations performed for each sample (sample generation, IR conversion, etc).
This can be used to identify performance bottlenecks in the fuzzer.

The summaries can be read with the tool
`//xls/fuzzer/read_summary_main`. See usage description in the code
for more details.

## Debugging a failing sample {#debugging}

A generated sample can fail in one of two ways: a tool crash or a result
miscomparison. A tool crash occurred if one of the tools invoked by the fuzzer
(e.g., `opt_main` which optimizes the IR) returned a non-zero status. A result
miscomparison occurred if there is not perfect correspondence between the
results produced by various ways in which the generated function is evaluated:

1.  Interpreted DSLX
1.  Evaluated unoptimized IR
1.  Evaluated optimized IR
1.  Simulation of the generated (System)Verilog

Generally, the results produced by the interpretation of the DSLX serves are the
reference results for comparisons.

To identify the underlying cause of the sample failure inspect the
`exception.txt` file in the crasher directory. The file contains the text of the
exception raised in `SampleRunner` which clearly identifies the kind of failure
(result miscomparison or tool crash) and details about which evaluation resulted
in a miscompare or which tool crashed, respectively. Consult the following
sections on how to debug particular kinds of failures.

### Debugging a tool crash

The `exception.txt` file includes the invocation of the tool for reproducing the
failure. Generally, this is a straightforward debugging process.

If the failing tool is the IR optimizer binary `opt_main` the particular pass
causing the failure should be in the backtrace. To retrieve the input to this
pass, run `opt_main` with `--ir_dump_path` to dump the IR between each pass. The
last IR file produced (the files are numbered sequentially) is the input to the
failing pass.

### Result miscomparison: unoptimized IR

The evaluation of the unoptimized IR is the first point at which result
comparison occurs (DSLX interpretation versus unoptimized IR evaluation). A
miscomparison here can indicate a bug in one of several places:

1.  DSLX interpreter
1.  DSLX to IR conversion
1.  IR interpreter or IR JIT. The error message in `exception.txt` indicates
    whether the JIT or the interpreter was used.

To help narrow this down, the IR interpreter can be compared against the JIT
with the `eval_ir_main` tool:

```
  eval_ir_main --test_llvm_jit --input_file=args.txt sample.ir
```

This runs both the JIT and the interpreter on the unoptimized IR file
(`sample.ir`) using the arguments in `args.txt` and compares the results. If
this is successful, then likely the IR interpreter and the JIT are correct and
problem lies earlier in the pipeline (DSLX interpretation or DSLX to IR
conversion). Otherwise, there is definitely a bug in either the interpreter or
the JIT as their results should *always* be equal.

If a minimized IR file exists (`minimized.ir`) this may be a better starting
point for isolating the failure.

### Result miscomparison: optimized IR

This can indicate a bug in IR evaluation (interpreter or JIT) or in the
optimizer. In this case, a comparison of the evaluation of the unoptimized IR
against the DSLX interpreter has already succeeds so DSLX interpretation or
conversion is unlikely to be the underlying cause.

As with miscomparison involving the _unoptimized_ IR, `eval_ir_main` can be used
to compare the JIT results against the interpreter results:

```
  eval_ir_main --test_llvm_jit --input_file=args.txt sample.opt.ir
```

If the above invocation fails there is a bug in the JIT or the interpreter.
Otherwise, there may be a bug in the optimizer. The tool `eval_ir_main` can help
isolate the problematic optimization pass by running with the options
`--optimize_ir` and `--eval_after_each_pass`. With these flags, the tool runs
the optimization pipeline on the given IR and evaluates the IR after each pass
is run. The first pass which results in a miscompare against the unoptimized
input IR is flagged. Invocation:

```
  eval_ir_main --input_file=args.txt \
    --optimize_ir \
    --eval_after_each_pass \
    sample.ir
```

#### Debugging the LLVM JIT

To help isolate bugs in the JIT, LLVM's optimization level can be set using the
`--llvm_opt_level` flag:

```
  eval_ir_main --test_llvm_jit \
    --llvm_opt_level=0 \
    --input_file=args.txt sample.opt.ir
```

If the results match (pass) with the optimization level set to zero but fail
with the default optimization level of 3, there is likely a bug in the LLVM
optimizer or the XLS-generated LLVM program has undefined behavior.

Unoptimized and optimized LLVM IR are dumped by the JIT with vlog level of 2 or
higher, and the assembly is dumped at level 3 or higher. For example:

```
  eval_ir_main -v=3 --logtostderr --random_inputs=1 sample.opt.ir
```

Extract the unoptimized LLVM IR to file to enable working with it in isolation.
Copy the text following `Unoptimized module IR:` or `Optimized module IR:`
depending upon whether or not the unoptimized or optimized IR is desired. Copy
into `sample.ll`. Execute the following command to remove the logging prefix
text.

```
sed -i 's/^.*\] //g' sample.ll
```

##### Building LLVM tools

The various LLVM tools such as `opt` and `lli` can be built with:

```
  bazel build /llvm/llvm-project/llvm:all
```

Build in fastbuild mode to get checks and debug features in LLVM.

##### Running the LLVM optimization passes

To run the LLVM IR optimizer run the following (starting with the *unoptimized*
IR):

```
  opt sample.ll -O3 -S
```

To print the IR before and after each pass:

```
 opt sample.ll -S -print-after-all -print-before-all -O3
```

##### Running the Instcombine pass

Instcombine is an LLVM optimization pass which is a common source of bugs in
code generated from XLS. To run instcombine alone:

```
 opt /tmp/bad.ll -S -passes=instcombine
```

Instcombine is a large monolithic pass and it can be difficult to isolate the
exact transformation which caused the problem. Fortunately, this pass includes a
"compiler fuel" option which can be used to limit the number of transformations
performed by the pass. Example usage (fastbuild of LLVM is required):

```
opt -S --instcombine sample.ll --debug-counter=instcombine-visit-skip=0,instcombine-visit-count=42
```

##### Evaluating LLVM IR

The LLVM tool `lli` evaluates LLVM IR. The tool expects the IR to include a
entry function `main`. See the uploaded file in this
[LLVM bug](https://bugs.llvm.org/show_bug.cgi?id=45762) for an example LLVM IR
file which includes a main function that calls a (slightly modified)
XLS-generated function.

The LLVM tool `opt` optimizes the LLVM IR and can be piped to `lli` like so:

```
  opt sample.ll --O2 | lli
```

##### Running LLVM code generation

If the bug occurs during LLVM code generation (lowering of LLVM IR to object
code) the LLVM tool `llc` may be used to reproduce the problem. `llc` takes LLVM
IR and produces assembly or object code. Example invocation for producing object
code:

```
llc sample.ll -o sample.o --filetype=obj
```

The exact output of `llc` depends on the target machine used during compilation.
Logging in the OrcJit (at vlog level 1) will emit the exact `llc` invocation
which uses the same target machine as the JIT.

### Result miscomparison: simulated Verilog

This can be a bug in codegen, XLS's Verilog testbench code, or the Verilog
simulator itself. Running the generated Verilog with different simulators can
help isolate the problem:

```
  simulate_module_main --signature_file=module_sig.textproto \
    --args_file=args.txt \
    --verilog_simulator=iverilog \
    sample.v

  simulate_module_main --signature_file=module_sig.textproto \
    --args_file=args.txt \
    --verilog_simulator=${SIM_2} \
    sample.v
```

The tool outputs the results of the evaluation to stdout so diffing their
outputs is required.

### Filing an LLVM bug

If the fuzzer problem is due to a crash or miscompile by LLVM, file an LLVM bug
[here](https://github.com/llvm/llvm-project/issues).  Example LLVM bugs found by
the fuzzer: [1](https://github.com/llvm/llvm-project/issues/61127),
[2](https://github.com/llvm/llvm-project/issues/61038).

Although the internal Google mirror of LLVM is updated frequently, prior to
filing an LLVM bug it's a good idea to verify the failure against LLVM head.
Steps to build a debug build of LLVM:

```
git clone https://github.com/llvm/llvm-project.git
cd llvm-project
mkdir build
cd build
cmake -G Ninja ../llvm -DCMAKE_BUILD_TYPE=Debug -DLLVM_TARGETS_TO_BUILD=X86
cmake --build . -- opt # or llc or other target.
```

Below are instructions to configure LLVM with sanitizers enabled. This can be
useful for reproducing issues found with the sanitizer-enabled fuzz tests.

```
# Install a version of LLVM which supports necessary sanitizer options.
sudo apt-get install lld-15 llvm-15 clang-15 libc++1-15
# In llvm-project directory:
mkdir build-asan
cd build-asan
TOOLBIN=/usr/lib/llvm-15/bin
# Below enables a particular sanitizer option `sanitize-float-cast-overflow`.
# `Address` can be used as an option instead of `Undefined` depending on the
# desired sanitizer check.
cmake ../llvm -GNinja -DCMAKE_BUILD_TYPE=RelWithDebInfo \
  -DCMAKE_CXX_COMPILER=$TOOLBIN/clang++ -DCMAKE_C_COMPILER=$TOOLBIN/clang \
  -DLLVM_USE_SANITIZER=Undefined \
  -DLLVM_UBSAN_FLAGS='-fsanitize=float-cast-overflow -fsanitize-undefined-trap-on-error' \
  -DLLVM_ENABLE_LLD=On -DLLVM_TARGETS_TO_BUILD=X86
```

LLVM includes a test case minimizer called
[`bugpoint`](https://llvm.org/docs/Bugpoint.html) which tries to reduce the size
of an LLVM IR test case. `bugpoint` has many options but it can operate in a
similar manner to the XLS IR minimizer where a user-specified script is used to
determine whether the bug exists in the LLVM IR:

```
bugpoint input.ll -compile-custom -compile-command bugpoint_test.sh
```

Example bugpoint test script (`bugpoint_test.sh`):

```shell
#!/bin/bash

# Create a temporary file for the test command
logfile="$(mktemp)"

# Run your test command (and redirect the output messages)
/path/to/llc "$@" -o /tmp/out.o -mcpu=skylake-avx512 --filetype=obj > "${logfile}" 2>&1
ret="$?"

# Print messages when error occurs
if [ "${ret}" != 0 ]; then
  echo "test failed"  # must print something on failure
  cat "${logfile}"
fi

# Cleanup the temporary file
rm "${logfile}"

exit "${ret}"
```
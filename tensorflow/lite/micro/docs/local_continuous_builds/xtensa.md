<!-- mdformat off(b/169948621#comment2) -->

<!--
Semi-automated TOC generation with instructions from
https://github.com/ekalinin/github-markdown-toc#auto-insert-and-update-toc

gh-md-toc --insert --no-backup xtensa.md
-->

<!--ts-->
   * [Summary](#summary)
   * [Detailed Results](#detailed-results)
      * [Hifimini](#hifimini)
         * [Unit tests](#unit-tests)
         * [Keyword Benchmark](#keyword-benchmark)
            * [Binary size graph](#binary-size-graph)
            * [Latency graph](#latency-graph)
      * [Fusion F1](#fusion-f1)
         * [Unit tests](#unit-tests-1)
         * [Keyword Benchmark](#keyword-benchmark-1)
            * [Binary size graph](#binary-size-graph-1)
            * [Latency graph](#latency-graph-1)

<!-- Added by: advaitjain, at: Wed 09 Dec 2020 04:09:52 PM PST -->

<!--te-->

# Summary

* Overall status (as seen on the tensorflow repo): ![Status](xtensa-build-status.svg)

* Table with more granular results

| Architecture |  Keyword benchmark (build) | Unit tests |
| ---------- |       -------              |  --------  |
| Hifimini  | ![Status](xtensa-hifimini-keyword-build-status.svg) | ![Status](xtensa-hifimini-unittests-status.svg) |
| Fusion F1  | ![Status](xtensa-fusion_f1-keyword-build-status.svg) | ![Status](xtensa-fusion_f1-unittests-status.svg) |
| Hifi4  | | |


# Detailed Results

## Hifimini

### Unit tests
* [Unit test build log](hifimini_unittest_log) from the most recent run.
* [Unittest status history](hifimini_unittest_status)

### Keyword Benchmark

* [Keyword benchmark build log](hifimini_build_log) from the most recent run.
* [Keyword benchmark build status history](hifimini_build_status)
* [Keyword Benchmark size history](hifimini_size_log)
* [Keyword Benchmark latency history](hifimini_latency_log)

#### Binary size graph
![Size graph](hifimini_size_history.png)

#### Latency graph
![Latency graph](hifimini_latency_history.png)

## Fusion F1

### Unit tests

* [Unit test build log](fusion_f1_unittest_log) from the most recent run.
* [Unittest status history](fusion_f1_unittest_status)

### Keyword Benchmark

* [Keyword benchmark build log](fusion_f1_build_log) from the most recent run.
* [Keyword benchmark build status history](fusion_f1_build_status)
* [Keyword Benchmark size history](fusion_f1_size_log)
* [Keyword Benchmark latency history](fusion_f1_latency_log)

#### Binary size graph
![Size graph](fusion_f1_size_history.png)

#### Latency graph
![Latency graph](fusion_f1_latency_history.png)



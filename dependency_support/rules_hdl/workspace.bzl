# Copyright 2021 The XLS Authors
#
# Licensed under the Apache License, Version 2.0 (the "License");
# you may not use this file except in compliance with the License.
# You may obtain a copy of the License at
#
#      http://www.apache.org/licenses/LICENSE-2.0
#
# Unless required by applicable law or agreed to in writing, software
# distributed under the License is distributed on an "AS IS" BASIS,
# WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
# See the License for the specific language governing permissions and
# limitations under the License.

"""Loads the rules_hdl package which contains Bazel rules for other tools that
XLS uses."""

load("@bazel_tools//tools/build_defs/repo:http.bzl", "http_archive")
load("@bazel_tools//tools/build_defs/repo:utils.bzl", "maybe")

def repo():
    # Required to support rules_hdl, 0.7.0 release is current as of 2022-05-09.
    http_archive(
        name = "rules_pkg",
        sha256 = "8a298e832762eda1830597d64fe7db58178aa84cd5926d76d5b744d6558941c2",
        urls = [
            "https://github.com/bazelbuild/rules_pkg/releases/download/0.7.0/rules_pkg-0.7.0.tar.gz",
            "https://mirror.bazel.build/github.com/bazelbuild/rules_pkg/releases/download/0.7.0/rules_pkg-0.7.0.tar.gz",
        ],
    )

    # Commit on 2022-10-08, current as of 2022-10-10
    git_hash = "ebaf7482c035208f485f463c62fd3c2f969a9b5c"
    git_sha256 = "3743f1ed6739abaaa68e1e907adffb13c285fd70390d950c3989729439d952c5"

    maybe(
        http_archive,
        name = "rules_hdl",
        sha256 = git_sha256,
        strip_prefix = "bazel_rules_hdl-%s" % git_hash,
        urls = [
            "https://github.com/hdl/bazel_rules_hdl/archive/%s.tar.gz" % git_hash,
        ],
    )
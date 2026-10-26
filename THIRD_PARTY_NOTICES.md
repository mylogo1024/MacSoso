# Third-Party Notices

MacSoSo links against the following libraries. Release builds bundle
their dynamic libraries inside `MacSoSo.app/Contents/Frameworks`.

## RE2

- Project: https://github.com/google/re2
- License: BSD 3-Clause
- Copyright (c) 2009 The RE2 Authors. All rights reserved.
- Full text: [third_party/re2-LICENSE](third_party/re2-LICENSE)

## Abseil

- Project: https://github.com/abseil/abseil-cpp
- License: Apache License 2.0
- Copyright 2017 The Abseil Authors.
- Full text: [third_party/abseil-LICENSE](third_party/abseil-LICENSE)

RE2 is linked as `libre2.11.dylib`; Abseil is pulled in transitively as the
individual `libabsl_*.dylib` files that RE2 requires.

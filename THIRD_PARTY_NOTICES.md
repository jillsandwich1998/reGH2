# Third-party software

This project contains no game data and no code generated from the game. It builds on:

* **ReXGlue SDK** (BSD-3-Clause, Tom Clay; portions Xenia, BSD-3-Clause) - recompiler, runtime and GPU backend. `sdk-patches/`
  holds our changes to it as patch files; the SDK itself is fetched from https://github.com/rexglue/rexglue-sdk at a pinned commit.
* **Forza Horizon 1: ReImagined** (MIT, Copyright (c) 2026 Dyotson and contributors) - `sdk-patches/0001` (spatial FSR/CAS without
  FidelityFX), `0006` (shared-memory uploads before the render pass) and `0007` (page-state, lock and memexport changes) adapt
  patches 0043, 0036, 0039, 0031 and 0049 of https://github.com/Dyotson/ForzaHorizon1-ReImagined.
* **AMD FidelityFX** FSR 1 / CAS shaders (MIT) - through the SDK.
* **Dear ImGui** (MIT), **SDL3** (zlib), **FFmpeg** (LGPL) - through the SDK.
* **XenonRecomp** (hedge-dev, MIT) - the codegen lineage the SDK builds on.

The AppImage additionally bundles clang/LLVM 20 (Apache-2.0 with LLVM exceptions), CMake (BSD-3-Clause), Ninja (Apache-2.0),
glibc / libstdc++ runtime headers and libraries (LGPL / GPL with runtime exception) and a portable CPython with Tk (PSF license).

Sources of the bundled system components (glibc, libstdc++, libgcc, X11 and Wayland client libraries) are the Ubuntu 22.04
packages of the same names (`apt source <package>`); LLVM, CMake, Ninja and CPython are available from their upstream projects.

## License texts

### ReXGlue SDK (BSD-3-Clause)

```
Copyright (c) 2026, Tom Clay <tomc@tctechstuff.com>

Portions of this software are derived from the Xenia project:
Copyright (c) 2022, Ben Vanik and Xenia project contributors
https://xenia.jp

All rights reserved.

Redistribution and use in source and binary forms, with or without
modification, are permitted provided that the following conditions are met:

1. Redistributions of source code must retain the above copyright notice, this
   list of conditions and the following disclaimer.

2. Redistributions in binary form must reproduce the above copyright notice,
   this list of conditions and the following disclaimer in the documentation
   and/or other materials provided with the distribution.

3. Neither the name of the copyright holder nor the names of its
   contributors may be used to endorse or promote products derived from
   this software without specific prior written permission.

THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS"
AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE
DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT HOLDER OR CONTRIBUTORS BE LIABLE
FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL
DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR
SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER
CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY,
OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
```

### Forza Horizon 1: ReImagined (MIT)

```
MIT License

Copyright (c) 2026 Dyotson and contributors

Permission is hereby granted, free of charge, to any person obtaining a copy of this software and associated documentation
files (the "Software"), to deal in the Software without restriction, including without limitation the rights to use, copy,
modify, merge, publish, distribute, sublicense, and/or sell copies of the Software, and to permit persons to whom the Software
is furnished to do so, subject to the following conditions:

The above copyright notice and this permission notice shall be included in all copies or substantial portions of the Software.

THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES
OF MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT HOLDERS BE
LIABLE FOR ANY CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM, OUT OF OR
IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.
```

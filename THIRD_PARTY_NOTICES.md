# Third-party notices

This project is MIT-licensed. Portions of it adapt algorithms from third-party
open-source software whose licenses are reproduced below.

## Instant Meshes (algorithm adaptation)

The quad-extraction stage of the position-field quadrangulator
(`src/quadrangulate/src/quad_extract.cpp`) is a clean-room reimplementation of
the mesh-extraction algorithm from *Instant Field-Aligned Meshes* (Jakob,
Tarini, Panozzo, Sorkine-Hornung, ACM Transactions on Graphics 34(6),
SIGGRAPH Asia 2015), as implemented in the reference project
[wjakob/instant-meshes](https://github.com/wjakob/instant-meshes). No source
code was copied; the algorithm is reimplemented independently. The reference
implementation is distributed under the following BSD-3-Clause license:

```
Copyright (c) 2015 Wenzel Jakob, Daniele Panozzo, Marco Tarini, and
Olga Sorkine-Hornung. All rights reserved.

Redistribution and use in source and binary forms, with or without
modification, are permitted provided that the following conditions are met:

1. Redistributions of source code must retain the above copyright notice, this
   list of conditions and the following disclaimer.

2. Redistributions in binary form must reproduce the above copyright notice,
   this list of conditions and the following disclaimer in the documentation
   and/or other materials provided with the distribution.

3. Neither the name of the copyright holder nor the names of its contributors
   may be used to endorse or promote products derived from this software
   without specific prior written permission.

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

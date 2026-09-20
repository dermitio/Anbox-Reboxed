# Licensing and attribution

Anbox Reboxed 0.0.1-devalpha is a modified, unofficial fork of Anbox.
The combined host project is distributed under GPL-3.0-only. The evidence
is the explicit version-3 grant in anbox/src/main.cpp and the inherited
core headers, together with anbox/COPYING.GPL. The upstream grant does not
say "or any later version":
https://github.com/anbox/anbox/blob/master/src/main.cpp

LICENSE reproduces the inherited GPLv3 text. New project-owned code and
documentation use GPL-3.0-only unless expressly marked otherwise.
Existing copyright notices and individual licenses remain controlling;
this statement does not relicense inherited or third-party files.
Retain anbox/AUTHORS and all existing notices when redistributing.

| Component | Applicable terms and retained evidence |
| --- | --- |
| Anbox core | GPL-3.0-only; anbox/COPYING.GPL and per-file headers |
| Anbox CLI and selected utilities | LGPL-3.0 as stated in their headers; full text in anbox/external/process-cpp-minimal/COPYING |
| process-cpp-minimal, xdg | LGPLv3; their COPYING/LICENSE files and headers |
| sdbus-c++ | LGPL-2.1-or-later per source headers; anbox/external/sdbus-cpp/COPYING and tools/COPYING |
| backward-cpp | MIT; anbox/external/backward-cpp/LICENSE.txt |
| cpu_features | Apache-2.0; anbox/external/cpu_features/LICENSE |
| Android/emugl, Android integration | Primarily Apache-2.0, with individual BSD/MIT and other notices where stated; retained headers, audio/NOTICE, and LICENSES/Apache-2.0.txt |
| nsexec uidmapshift | Separate GPL-2.0-only utility; its source header and LICENSES/GPL-2.0.txt |
| Legacy kernel modules | Separate GPL-2.0 components; anbox-modules source headers, debian/copyright, and LICENSES/GPL-2.0.txt; Debian packaging has its own GPLv3 grant |

The external libraries are build dependencies, not comparison-only source
trees. Their upstream tests and metadata are retained with their imports.
The separate GPLv2 utilities/modules are not relicensed as GPLv3 by the
root license.

Images, SDKs, platform signing material, and gfxstream builds are obtained
separately and retain their own terms. Unfinished Berberis and Native Bridge
support source imports are excluded from the public working tree.

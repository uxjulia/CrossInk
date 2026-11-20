#pragma once

// PlatformIO shares immutable generated tables. Standalone/host builds can
// use the original face headers, which also remain the input to the font IDs.
#ifdef CROSSINK_SHARED_BUILTIN_FONT_TABLES
#include <BuiltinFontsShared.generated.h>
#else
#include <builtinFonts/all_faces.h>
#endif

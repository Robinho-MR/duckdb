// Calls the generated static extension loader before main. Compile it into a program that wants its statically linked
// extensions registered without calling duckdb_register_static_extensions itself. Compile it into the program, not into
// an archive, or the linker drops it.

#include <stdint.h>

extern "C" int32_t duckdb_register_static_extensions(void);

namespace {
struct DuckDBStaticExtensionLoader {
	DuckDBStaticExtensionLoader() {
		duckdb_register_static_extensions();
	}
};
// registering the statically linked extensions has to happen before main
#if defined(__clang__)
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wglobal-constructors"
#endif
const DuckDBStaticExtensionLoader duckdb_static_extension_loader;
#if defined(__clang__)
#pragma clang diagnostic pop
#endif
} // namespace

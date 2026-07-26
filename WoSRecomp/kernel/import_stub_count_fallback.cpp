// Used only when kernel/imports_generated.cpp hasn't been generated, so the
// host still links. Reports zero stubs, which DumpImportLog handles.
#include <cstddef>
size_t WoSImportStubCount() { return 0; }

// The files folder the sentinel's log and config use (sentinel::FilesDir, sentinel_log.cpp), for the combat host tests that link a module whose
// production install names the folder where the q15t plugin kept its switch files. Weak: a test that names a folder of its own defines the symbol
// itself and wins. Nothing exists at this path, so no legacy switch file is ever found through it.
#include "quest/sentinel/sentinel_log.h"

namespace sentinel {

__attribute__((weak)) const char* FilesDir() { return "/nonexistent/nevr-files-for-host-tests"; }

}  // namespace sentinel

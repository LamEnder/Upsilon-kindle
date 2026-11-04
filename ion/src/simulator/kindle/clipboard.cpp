#include <ion/clipboard.h>

/* There is no system clipboard to share with: Escher keeps its own copy of
 * whatever was cut/copied, which is all we need. */

namespace Ion {
namespace Clipboard {

void write(const char * text) {
}

const char * read() {
  return nullptr;
}

}
}

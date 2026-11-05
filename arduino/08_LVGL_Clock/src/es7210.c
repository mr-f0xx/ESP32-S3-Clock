/* Arduino builds sources under src/ recursively, but not arbitrary sketch
 * subfolders. Compile the vendored ES7210 driver here while keeping its
 * upstream directory layout and license intact. */
#include "../third_party/es7210/es7210.c"

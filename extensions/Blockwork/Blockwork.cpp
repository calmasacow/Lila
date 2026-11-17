// Blockwork: grid-first level blocking. Node types and commands; the Blockwork tool and panels are their UI.

#include "Blockwork.h"

LILA_EXTENSION_EXPORT int lilaExtensionInit(lila::ext::Context& ctx)
{
    if (!blockwork::registerTypes(ctx))
        return 1;
    blockwork::registerCommands(ctx);
    return 0;
}

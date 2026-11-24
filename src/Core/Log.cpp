#include <Core/Log.h>

using namespace CZ;

const CZLogger &GazetteLogger() noexcept
{
    static CZLogger logger { "Gazette", "CZ_GAZETTE_LOG_LEVEL" };
    return logger;
}

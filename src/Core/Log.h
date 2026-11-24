#ifndef GAZETTE_LOG_H
#define GAZETTE_LOG_H

#include <CZ/Core/CZLogger.h>

/// Gazette's logger (CZ_GAZETTE_LOG_LEVEL).
#define Log GazetteLogger()

/// See Log.
const CZ::CZLogger &GazetteLogger() noexcept;

#endif // GAZETTE_LOG_H

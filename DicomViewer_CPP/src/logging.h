#ifndef LOGGING_H
#define LOGGING_H

#include <QtCore/QDebug>

// Simple conditional logging that completely removes debug output in Release mode
#ifndef NDEBUG
#else
    #define LOG_DEBUG(msg) do { } while (0)
#endif

#endif // LOGGING_H
